# Phase 0: ps3recomp and XenonRecomp evaluation

Oct 3, 2026 · Phase 0 item "Evaluate PS3Recomp" (plan §10) and the open "Toolchain" decision (plan §14), plus a check
of D-004 and D-009 against XenonRecomp.

**Method.** Static reading only: shallow clones (`--depth 200`), `git log`, GitHub API metadata, grep. Nothing from
either repository was built, configured or run. Snapshots: ps3recomp `a679051` (master, 2026-09-29), XenonRecomp
`ddd128b` (main, 2025-08-04). RPCS3 master `0ca1c58e75` files were read through the GitHub API for cross-checks.
Prefixes in citations: `pr:` ps3recomp, `xr:` XenonRecomp, `rp:` RPCS3; paths without a prefix are in this repo.
**[S]** marks speculation.

**Not done:** "output quality on the GTA IV ELF". There is no GTA IV dump yet (D-002), and running ps3recomp's Python
lifter means executing untrusted code. Quality was judged from the emitter source and the generated code committed in
the repo. Section 4 says whether that run is still worth doing.

## Summary

- **Toolchain decision: write our own**, as the plan assumes. Use XenonRecomp as the design reference for the PPU
  emitter, RPCS3 for semantics (D-001 allows it), and ps3recomp only as a catalogue of PS3 pitfalls and synthetic test
  ideas. Reuse neither ps3recomp's lifter nor its runtime.
- **D-004 (full 16-byte reverse): confirmed.** XenonRecomp and RPCS3 use the same layout. Extend the list of ops that
  need index translation (§3).
- **D-009 (signature): shape confirmed, two changes suggested.** Drop `restrict` on `base`, and make guest memory
  accesses in lifted code non-elidable (XenonRecomp uses `volatile`). Add an alignment assumption on `base` (§3).

| | ps3recomp | XenonRecomp |
|---|---|---|
| Licence | MIT; provenance caveats (§1.1) | MIT; bundled disassembler GPL-2.0+ |
| As inbound code for GPLv2 | compatible; Sony firmware and keys in tree must not be copied | compatible; Xbox 360 key in tree, irrelevant |
| Age, activity | created 2026-03-11, 1,486 commits, daily pushes | created 2024-09-07, 181 commits, last push 2025-08-04 |
| People, review | 10 contributors; 664 of the latest 1,048 commits co-authored by Claude; 0 reviews on the last 60 merged PRs | 11 contributors; shipped in Unleashed Recompiled |
| PPU lifter | Python, pattern-matches disassembly text, emits C++ | C++, emits C++ with SSE intrinsics through SIMDe |
| Lifted signature | `void func_X(ppu_context* ctx)`, base is a global | `void sub_X(PPCContext& __restrict ctx, uint8_t* base)` |
| Guest memory access | out-of-line `vm_read*`/`vm_write*` calls with instrumentation | inline `volatile` load/store + byte swap |
| Registers in locals | never | opt-in, ABI-based |
| VMX lane order | raw big-endian bytes (opposite of D-004); SPU uses a third layout | full 16-byte reverse (= D-004) |
| CR, XER | packed `uint32_t` | one byte per CR bit, one byte per XER flag |
| FPSCR, VMX denormals | FPSCR unmodelled, NJ ignored | rounding mode + flush-to-zero toggled around VMX |
| Indirect calls | hash table; unknown target logged, then skipped | direct-indexed table, one load |
| Unknown instruction | `/* TODO */` no-op in the output | warning at lift time, no-op in the output |
| SPU | Python lifter + interpreter | none |
| Runtime | lv2 (~164 syscalls), HLE (~93k lines), RSX; games run on Windows only | none |
| Tests | Python reference semantics (integer + ~15 VMX cases), log-diff gate on 5 ports | Xenia's PPC instruction tests |

## 1. ps3recomp (github.com/sp00nznet/ps3recomp)

### 1.1 Licence and provenance

- MIT, "Copyright (c) 2026 Ned Heller / sp00nznet" (`pr:LICENSE`). MIT is GPLv2-compatible: keep the copyright and
  permission notice on anything taken. For reference: BSD-2/3-Clause and zlib are also compatible; Apache-2.0 is not
  (only GPLv3 accepts it); code with no licence cannot be used at all.
- Provenance is inconsistent. The README says the HLE libraries are "from RPCS3's module system"
  (`pr:README.md:161`) and lists RPCS3's HLE modules and "XenonRecomp's PowerPC lifter, adapted" as building blocks
  (`pr:README.md:391-392`); CONTRIBUTING forbids porting GPL code (`pr:CONTRIBUTING.md:245-250`). The
  `patches/xenonrecomp-ppu.patch` the README lists (`pr:README.md:190-191`) does not exist, and the lifter is an
  independent Python implementation. Under D-001 either origin is usable; anything taken should credit ps3recomp and
  be checked against RPCS3.
- Most recent code is AI-assisted: 664 of the 1,048 most recent commits carry `Co-Authored-By: Claude …` trailers.
  **[S]** The copyright status of purely machine-generated code is unsettled; low risk for a private GPLv2 project.
- The README's legal statement ("does not contain any proprietary Sony code, encryption keys…", `pr:README.md:415`)
  is not accurate; see §1.7.
- Third party: `pr:third_party/at3_standalone` is LGPL-2.1, which is GPLv2-compatible. The Metal shader path links
  glslang and SPIRV-Cross (`pr:libs/video/rsx_shader_msl.cpp`); SPIRV-Cross is Apache-2.0, so that path cannot go into
  a GPLv2 app.

### 1.2 Maturity

- Created 2026-03-11, 1,486 commits on master, 353 stars, 29 forks. Contributors (GitHub API): sp00nznet 924 commits,
  canersaka 257, sagemono 244, "claude" 41, six others with 8 or fewer.
- It started as large drops: the initial commit `c6aa277` added 11,675 lines, and `338803c` added 17,826 lines
  ("33 complete HLE modules") 19 minutes later.
- PRs: 104 merged, 84 closed unmerged, 7 open. Issues: 5 open, 7 closed. The last 60 merged PRs have no GitHub reviews
  and add 103,360 lines, all merged by the owner, several within the hour (#142, +12,352 lines, 7 min; #140, +18,381,
  40 min; #171, +33,067, 90 min).
- Size ≈236k lines in total, including Python tools 30k, runtime 47k, HLE libs 93k, committed generated SPU C 33k and
  docs 10k.
- Results claimed in the README (not verified): games run on **Windows/D3D12 only**. The Simpsons Arcade is playable,
  Tokyo Jungle reaches its menus, You Don't Know Jack reaches gameplay, VF5 shows loading screens, and Rubber Ducky has
  regressed to zero draws (`pr:README.md:368-374`). macOS arm64 builds and passes the CI suites but "Runs a recompiled
  game: no" (`pr:README.md:49-62`). Nothing for iOS.
- It works like a research harness: the runtime and libs read 614 distinct environment variables, 74 of them named
  after a title (`YDKJ_*`, `FLOW_*`, `LBP_*`). Title fixes live in shared runtime code: a native memmove replaces a
  mis-lifted guest function at a fixed address (`pr:runtime/ppu/ppu_loader.cpp:2025-2056`), and a flOw probe rewrites
  the argument of calls to `0x000DBD84` when `FLOW_FORCE_RTCFG` is set (`:2448`).

### 1.3 Architecture

**PPU lifter** (`pr:tools/ppu_lifter.py`, 4,587 lines of Python)

- Translates disassembler text (mnemonic and operand strings) into C statements through a long `if mn == …` chain
  (`:862-2989`). The output is C++ (`extern "C"` in the preamble, `:327-338`), split into chunk files.
- One C function per guest function, with `goto loc_X` for internal branches. Branches to other functions or to
  mid-function entries go through a thread-local trampoline (`g_trampoline_fn = func_X; return;`), drained after every
  call (`DRAIN_TRAMPOLINE`, `:346`; `b` at `:1391-1417`, `bl` at `:1419-1431`). Each mid-function entry is lifted again
  as a separate function running to the end of the original, so code is duplicated.
- Signature `void func_XXXXXXXX(ppu_context* ctx)` (`:3342`). The guest base is a global, `vm_base` (`:327`).
- No register caching: every access is `ctx->gpr[n]`, `ctx->cr` or `ctx->xer`.
- Memory accesses are calls to extern `vm_read*`/`vm_write*` (`:115-122`). The runtime implements them out of line with
  bounds checks, watchpoints, NULL-read reports, raw-SPU MMIO checks and a GCM polling hook
  (`pr:runtime/ppu/ppu_loader.cpp:1520-1610`). Out-of-range reads return 0 and out-of-range writes are dropped
  (`:1560`, `:1872`).
- Context: CR packed into one `uint32_t`, XER packed, an FPSCR field that nothing writes
  (`pr:runtime/ppu/ppu_context.h`).
- Jump tables: GCC TOC-relative idioms are found by `discover_jump_tables` (`:3507-3920`) and emitted as
  `switch ((uint32_t)ctx->ctr) { case 0x…: goto loc_…; default: ps3_indirect_call(ctx); return; }` (`:1499-1530`).
- Indirect calls: `bctrl` becomes `ps3_indirect_call(ctx)` (`:1532-1536`), backed by an open-addressing hash with 2^18
  slots (`pr:runtime/ppu/ppu_loader.cpp:1951-2013`).
- VMX: registers hold raw big-endian bytes (`:250-256`). `lvx` is a memcpy (`:2178-2190`), `vperm` a byte loop with
  direct indexing (`:2256-2269`), `vsldoi` a memcpy from a 32-byte temporary (`:2405-2412`). Ops that read lane values
  swap each lane through scalar loops (`ppu_vldf4` and friends, `:260-275`). VSCR[NJ] is not modelled.
- FP: explicit PPC NaN-propagation helpers and a real `fma()` for the `fmadd` family (`:178-248`, `:1701-1712`);
  saturating `fctiw*`; `fres` and `frsqrte` computed exactly rather than as estimates; `mtfsf` and `mtfsb*` ignored, so
  `mffs` returns a field nothing writes (`:1939-1954`).
- Atomics and ordering: `lwarx`/`stwcx.` are a compare-and-swap on the reserved word's value (`:1983-2018`). `sync`,
  `lwsync`, `eieio` and `isync` become seq_cst, acq_rel, release and acquire fences (`:2103-2125`). `mftb` reads a
  runtime clock scaled to 79.8 MHz (`:2076-2084`), and `dcbz` zeroes 128 bytes (`:2088-2092`).
- Function discovery: `.opd` descriptors, prologue and epilogue patterns and `bl` targets
  (`pr:tools/find_functions.py`), optionally merged with IDA or Ghidra exports.

**SPU** (`pr:tools/spu_lifter.py`, 1,248 lines; `pr:runtime/spu/`)

- One C function per SPU function over `spu_context*`, with instruction semantics as inline helpers
  (`pr:runtime/spu/spu_helpers.h`). The SPU interpreter (`pr:runtime/spu/spu_interp.c`) runs the same helpers, so it is
  not an independent oracle.
- Register layout: host-endian words in guest word order, so `_u32[0]` is the preferred slot and the bytes inside each
  word are reversed (`SPU_W(P)`, `spu_helpers.h:51-57`). That is a third convention, different from D-004 and from
  ps3recomp's own VMX layout.
- DMA is a synchronous memcpy. Every lock-line operation, PPU or SPU, takes one process-wide spinlock
  (`pr:runtime/spu/spu_coherency.h:37-42`; busy spin in `spu_coherency.c:55`). PPU stores check a bitmap of
  SPU-reserved lines so that they can raise the reservation-lost event (`spu_coherency.h:1-22`).
- SPURS: HLE in `pr:libs/spurs/cellSpurs.c` (3,238 lines), plus an LLE path that lifts the user's own `libsre.prx`.
  They added LLE because HLE SPURS "has been a source of endless problems" (`pr:docs/FIRMWARE_LLE.md`, "Why LLE").

**Runtime**

- lv2: about 164 syscall registrations covering threads, mutex/cond/rwlock/semaphore, event queue/port/flag, timers,
  memory/mmapper/vm, fs, SPU thread groups, raw SPU, tty, process and `sys_rsx` (`pr:runtime/syscalls/*.c`,
  `pr:runtime/spu/spu_raw.c`, `pr:libs/video/sys_rsx.c`). Unknown syscalls log and return `CELL_ENOSYS`
  (`pr:runtime/syscalls/lv2_syscall_table.h:263-268`).
- HLE: `pr:libs/` has 247 files, about 93k lines, across audio, codec, filesystem, font, input, network, SPURS, sync,
  system, video and misc; the README claims 97 complete modules (`pr:README.md:266-298`). Every module plan §8 expects
  has a file, for example cellGcmSys (3,456 lines), cellSpurs (3,238), cellSaveData (1,515), cellAudio (1,119),
  cellPad (1,108), cellFs (963), cellSync (778), cellResc (304) and cellVideoOut (255). A rough regex scan found about
  1,090 `cell*`/`sys*`/`sce*` definitions, 45 of which only return `CELL_OK`; how faithful the rest are was not
  assessed.
- HLE calls go through a generic adapter that passes eight GPRs; pointer arguments arrive as raw guest addresses that
  every handler must translate (`pr:runtime/ppu/ppu_hle.cpp:9-16`).
- Unresolved NIDs return `CELL_OK` by default (`pr:runtime/ppu/ppu_hle.cpp:124-178`).
- The guest memory map is simplified (`pr:runtime/memory/vm.h:7-12`), and the runtime's own GCM window has collided
  with two games' heaps (`vm.h:53-77`). Plan §4 takes the map from RPCS3 instead.
- The host layer is shaped after Win32 (SRWLOCK, `GetTickCount`, `RtlCaptureStackBackTrace`), with POSIX shims
  (`pr:runtime/platform/win32_compat.h`).

**RSX**

- A FIFO/NV4097 parser and a draw engine (`pr:libs/video/rsx_commands.c`, `rsx_draw_engine.c`).
- The D3D12 backend (362 KB) is the only one that runs games. The Metal backend (`rsx_metal_backend.m`, 155 KB)
  records draws and replays them at present time.
- Shaders: NV40 microcode → HLSL (`rsx_fp_decompiler.c`, field layout taken from Mesa/nouveau) → glslang → SPIR-V →
  SPIRV-Cross → MSL (`rsx_shader_msl.cpp`).

**NID database.** A NID is the first four bytes of SHA-1(name ‖ a fixed 16-byte suffix), read little-endian
(`pr:tools/nid_database.py:39-55`). The table has about 200 built-in names plus a harvested JSON, and
`pr:tools/gen_hle_nids.py` emits the handler table. Easy to regenerate from RPCS3's module tables.

**Loaders.** `pr:tools/elf_parser.py` parses ELF and SELF headers without decrypting. `pr:tools/unfself.py` unwraps
fSELF files and needs no keys. `pr:tools/ppu_loader.py` extracts OPD, TOC and imports. PRX relocation lives in
`pr:tools/lift_prx.py`, `pr:tools/prx_relocate.py` and `pr:runtime/prx/prx_loader.c`. The parts that decrypt with keys
are listed in §1.7.

**Testing**

- `pr:tools/test_ppu_lift.py` compiles lifter output with the host compiler and checks it against reference semantics
  written in Python. Scope: "tranche 1" integer ALU plus about 15 VMX cases (`:1-28`, `:605-690`).
- Synthetic unit vectors (jump tables, branches, fallthrough), SPU helper tests, sync and fiber stress tests, and a
  hand-written "smoke title" that drives the boot path on Metal (`pr:runtime/ppu/tests/smoke/smoke_recomp.c`).
- A regression gate diffs a milestone event stream of five Windows ports against recorded goldens
  (`pr:docs/REGRESSION_GATE.md`).
- A guest torture suite needs Sony's `ppu-lv2-gcc` (`pr:tests/run_torture.py:1-14`).
- There is no PPU interpreter, so no instruction-level differential testing, and ps3autotests is not used. RPCS3 is
  consulted by hand through its GDB stub (`pr:tools/rpcs3_probe/`).
- CI runs on Windows, macOS arm64 and Linux, including the lifter suites under Apple clang
  (`pr:.github/workflows/macos.yml:54-62`).

### 1.4 Output quality

**Apple clang, arm64:** compiles, going by CI (statement-level conformance cases and the hand-written smoke title),
with `-fno-strict-aliasing` and `-ffp-contract=off` (`pr:templates/project/CMakeLists.txt:232`, `:255-264`). Not
checked here on a real lift.

Correctness problems found by reading the code:

- **Silent fallbacks** (our rules 4 and 5 forbid these):
  - Lift time: an unknown instruction becomes a `/* TODO */` no-op (`pr:tools/ppu_lifter.py:2989`); an unknown branch
    condition becomes the constant `1`, so the branch is always taken (`:3023`); an unsupported SPR is a no-op
    (`:1589`); traps are dropped (`:2021`); a missing continuation becomes `return` (`:3376`). A committed audit
    baseline counts 6,021 `.word` TODOs in one title (`pr:scratch/lift_audit_baseline.txt`).
  - Runtime: an unresolved NID returns `CELL_OK`. An unresolved indirect call is logged and execution continues; the
    run aborts only after 2,000 identical repeats (`pr:runtime/ppu/ppu_loader.cpp:2724-2757`). A call target in
    0x10000000–0x30000000 becomes a no-op that returns r3 = 0 (`:2686-2722`). Swapped OPD words are "fixed"
    automatically (`:2622-2637`), and the recursion cap returns silently (`:2639-2646`). Unsupported SPU opcodes warn
    64 times and then keep running (`pr:runtime/spu/spu_helpers.h:73-85`).
- **Heuristic rewriting of lifted code.** Callee-saved restores are replaced by snapshots taken at function entry, to
  survive "frame drift" (`pr:tools/ppu_lifter.py:666-845`). The comments record at least four miscompiles caused by
  this heuristic itself, and open PR #207 changes it again. `mtctr` truncates CTR to 32 bits (`:1551-1556`).
- **VMX lane bugs.** Several handlers read multi-byte lanes of the raw big-endian registers through host-endian
  pointers, so on any little-endian host they compute on byte-swapped values:
  - `vmsumshm` (`:2415`), and the `vmsummbm` accumulator (`:2979`);
  - `vadduhs`, `vaddshs`, `vadduws` and `vsubsws` (`:2703-2724`), and `vsum2sws` (`:2732`);
  - `vmulouh` and `vmuleuh` (`:2836-2846`);
  - `vpkshss`, `vpkuhus`, `vpkuwus` and `vpkuwum` (`:2940-2977`).

  The conformance suite covers none of them. The same class of bug has already been fixed one opcode at a time:
  `vadduwm`, `vmax*`, `vsraw`, `vrlw`, `vmulosh` and `vupk*` (comments at `:2502-2508`, `:2549-2553`, `:2820-2826`,
  `:2897-2906`).
- **Stale generated SPU code.** The committed output is stale against the current helpers: it splats the link
  register, which `spu_helpers.h:63-66` calls "the old bug". One lift starts at an ELF header
  (`pr:fw_spu/spu_libsre/lifted/spu_recomp.c:7`).

Performance patterns, not measured (**[S]** for the size of the effect):

- every guest load and store is an out-of-line call through about ten checks;
- every register lives in memory and is reloaded around those calls;
- every call is followed by a thread-local trampoline check, and tail calls go through a thread-local function pointer;
- VMX runs as scalar byte loops;
- every indirect call is a hash lookup behind diagnostics gated by `getenv`;
- per a CMake comment, lifted translation units are built at `-O1` because they are so large
  (`pr:templates/project/CMakeLists.txt:173`).

**[S]** Expect output several times slower than XenonRecomp-style code; it cannot meet the 22 ms budget without
rewriting the emitter and the accessors, which amounts to a new lifter.

### 1.5 Fit with pspocket

| Our design | ps3recomp | Fit |
|---|---|---|
| D-004 full reverse | VMX raw big-endian; SPU per-word swap | no: every byte- and element-indexed op differs |
| D-009 `f(ctx, base)`, registers in locals | `func(ctx)`, global base, no locals | no |
| Oracle before optimizer (rule 3) | no PPU interpreter | no |
| Fail loudly (rules 4, 5) | defaults fake success | no |
| No title code in the runtime (rule 6) | title fixes and 74 title-named env vars in the runtime | no |
| macOS first, Metal | games only on Windows/D3D12; Metal untested on any game | no |
| No JIT, interpreter only as fallback | ahead-of-time; SPU interpreter fallback; no PPU fallback, unknown targets are skipped | partly |
| C++20 toolchain, C11 output (D-003) | Python toolchain, C++ output | no |
| Shared 128-byte reservation table (plan §6) | global spinlock; value CAS on the PPU | partly: the reservation-lost-on-store idea is useful |

### 1.6 Worth borrowing (as reference, with attribution)

| What | Where | Use |
|---|---|---|
| Real-game semantic pitfalls recorded in comments: `rlwinm` zero-extension, 64-bit `add`, `lmw`/`stmw`, update-form loads including `lfsu`, `cntlzw(0)`, `blrl` is a call, conditional `b*lr`, the `ld r2` TOC reload after cross-module calls, `vcmp*.` setting CR6, `lvlx`/`lvrx` | `pr:tools/ppu_lifter.py:898`, `:1032`, `:1049`, `:1192`, `:1623`, `:1388`, `:1434`, `:1160-1172`, `:2384`, `:2197` | checklist for our fuzz tests and interpreter |
| GCC jump-table idioms and synthetic vectors (no game data) | `pr:tools/ppu_lifter.py:3461-3920`, `pr:tools/test_ppu_jumptables.py` | analyzer test inputs |
| HLE behaviour found the hard way: `CellSyncMutex` is a big-endian ticket lock, the inbound mailbox is 4 deep, `sys_ppu_thread_once`, GCM control-register placement, the `SET_TRANSFORM_CONSTANT` 32-dword window (PR #208) | `pr:README.md:369-371`, `pr:runtime/spu/spu_context.h:120-140`, `pr:runtime/memory/vm.h:53-77` | leads to verify against RPCS3 |
| SPURS LLE experience | `pr:docs/FIRMWARE_LLE.md` | input to the SPURS decision (plan §14) |
| PPU stores raising the SPU reservation-lost event | `pr:runtime/spu/spu_coherency.h` | design input for the reservation table (plan §6) |

Code to copy verbatim: none.

### 1.7 Do not copy (legal)

- Decrypted Sony firmware: `pr:fw_spu/libadec.prx`, `libat3dec.prx`, `libatrac3multi.prx`, `libatrac3plus.prx`,
  `libatxdec.prx` and `libsre.prx` (plain ELF with type `0xffa4`, so already decrypted PRX), plus about 33k lines of C
  lifted from them under `pr:fw_spu/*/`. Their `.gitignore` blocks `*.sprx` but not `*.prx`.
- Key material: PKG AES keys in `pr:tools/pkg_extract.py:17` and `:20`; SDAT/EDAT keys in
  `pr:tools/extract_jobmods.py:23-24` and `pr:runtime/ppu/sdata_decrypt.h:132-133`, the latter used by
  `cellFsSdataOpen` (`pr:runtime/ppu/ppu_fs.cpp:20`).
- Title-specific code and addresses in `pr:runtime/ppu/ppu_loader.cpp` and `pr:libs/`.

The evaluation clone lives in the session scratchpad, outside this repo. Nothing from it was copied here.

## 2. XenonRecomp (github.com/hedge-dev/XenonRecomp)

### 2.1 Licence and status

- MIT (`xr:LICENSE.md`). The bundled PPC disassembler is binutils code under GPL-2.0-or-later
  (`xr:thirdparty/disasm/ppc-dis.c:1-20`), which D-001 allows.
- Submodules: SIMDe (MIT), fmt, xxHash, toml++, tiny-AES-c, libmspack. `xr:XenonUtils/xex.h:5` holds the Xbox 360 retail
  XEX key: irrelevant to us, do not copy.
- 181 commits, 11 contributors, 6.5k stars, 75 open issues; last push 2025-08-04. Proven by the shipped Unleashed
  Recompiled port.
- It is a recompiler only, with no runtime (`xr:README.md:7`), and it requires Clang (`xr:README.md:252-254`).

### 2.2 Vector registers

- Layout: "the recompiler chooses to reverse the entire 16-byte vector" (`xr:README.md:19`). `PPCVRegister` is a
  16-byte-aligned union of lane arrays (`xr:XenonUtils/ppc_context.h:194-206`).
- `lvx` and `stvx`: an aligned 16-byte access at `(ra + rb) & ~0xF`, plus `shuffle_epi8` with `VectorMaskL` row 0,
  which is `0F 0E … 00` (`xr:XenonRecomp/recompiler.cpp:1136-1145`, `:1636-1642`; table at `ppc_context.h:547-565`).
- `lvsl` and `lvsr` load rows `VectorShiftTableL/R[sh]`, which hold guest byte indices in reversed positions
  (`recompiler.cpp:1166-1180`, `ppc_context.h:587-625`). `lvlx`/`lvrx` use `VectorMaskL/R[sh]`
  (`recompiler.cpp:1148-1164`). `stvlx`/`stvrx` are byte loops with reversed indices (`:1610-1634`). `stvewx` stores
  `u32[3 - ((ea & 0xF) >> 2)]` (`:1599-1607`).
- `vperm` indexes both sources with `15 - (c & 0xF)` and selects between them with bit 4 (`simde_mm_perm_epi8_`,
  `ppc_context.h:656-661`; `recompiler.cpp:1980-1983`).
- `vsldoi` is `alignr_epi8(vA, vB, 16 - sh)` (`recompiler.cpp:2100-2103`).
- Splats index with `15 - uimm`, `7 - uimm` and `3 - uimm` (`:2112-2144`). Merges swap their operands: `vmrghw` is
  `unpackhi(vB, vA)` (`:1929-1932`). `vmsum3fp128` sums the `yzw` lanes (`:1947-1950`).
- All VMX code uses SSE intrinsics, which SIMDe translates for ARM64 (`xr:README.md:23`).

### 2.3 Function signature, base and context

- `PPC_FUNC(x)` is `void x(PPCContext& __restrict ctx, uint8_t* base)` (`xr:ppc_context.h:27`). Every body starts with
  `__builtin_assume(((size_t)base & 0x1F) == 0)` (`:32`), and calls pass `(ctx, base)` (`:105`).
- Guest scalar loads and stores are `volatile` with `__builtin_bswap*` (`:35-84`), "to prevent Clang from doing unsafe
  code reordering" (`xr:README.md:17`).
- Each function is emitted as `__imp__sub_X` plus a weak `sub_X` wrapper, so a project can override any function and
  still call the original (`recompiler.cpp:2385-2451`, `xr:README.md:71-83`).
- Register caching: by default every register stays in `PPCContext`. Opt-in flags turn CTR, XER, CR, the reservation,
  the non-argument registers and the non-volatile registers into locals (`xr:README.md:55-69`,
  `recompiler_config.cpp:21-28`, `recompiler.cpp:268-353`).
- These flags trust the ABI: calls to the save/restore helpers are dropped entirely (`recompiler.cpp:391-394`). The
  README credits them with a binary about 20 MB smaller and frame times "reduced by several milliseconds" in Unleashed
  (`xr:README.md:69`).

### 2.4 Control flow

- Function discovery: boundaries from `.pdata` (`recompiler.cpp:177-180`) plus a basic-block walker
  (`xr:XenonAnalyse/function.cpp`). The TOML config can also give explicit `functions = [{address, size}]` and
  `invalid_instructions` (`xr:README.md:182-205`).
- Jump tables: XenonAnalyse matches four fixed compiler patterns (absolute, computed, byte offset, short offset) and
  writes them to a TOML file (`xr:XenonAnalyse/main.cpp:10-120`). The recompiler emits
  `switch (rN.u64) { case i: goto loc_X; … default: __builtin_unreachable(); }` (`recompiler.cpp:609-641`; config in
  `recompiler_config.cpp:72-90`).
- Direct calls are C calls. A branch out of the function becomes a call followed by `return`, which relies on clang to
  turn it into a tail call (`recompiler.cpp:597-607`, `:372-405`). A call to an unknown target emits only a `// ERROR`
  comment (`:402`). Unrecognized instructions print a warning at lift time and emit nothing (`:2432`).
- Indirect calls: `PPC_CALL_INDIRECT_FUNC(ctr)` loads a host function pointer from a table placed after the image in
  guest address space, indexed by `(addr - code_base) * 2` (`ppc_context.h:110-114`, `xr:README.md:29-31`).
- `blrl` becomes `__builtin_debugtrap()` (`recompiler.cpp:714-716`). Exceptions are unsupported, and
  `setjmp`/`longjmp` are redirected to the host (`xr:README.md:47-53`).

### 2.5 CR and XER

- CR: eight `PPCCRRegister` structs, each with one byte for lt, gt, eq and so, plus typed `compare` helpers
  (`ppc_context.h:147-192`). `mfcr` and `mtcr` expand to 32 ternaries or assignments (`recompiler.cpp:1235-1240`,
  `:1272-1278`).
- XER: three bytes, so, ov and ca (`ppc_context.h:140-145`).
- RPCS3 also keeps CR and XER unpacked (`rp:rpcs3/Emu/Cell/PPUThread.h:172-207`, `:233-243`).

### 2.6 FPSCR, rounding, denormals

- `PPCFPSCRRegister` only mirrors the host control register. `mffs` and `mtfsf` move the rounding mode in and out
  (`ppc_context.h:214-302`, `recompiler.cpp:1243-1245`, `:1284-1286`); no exception or status bits are modelled.
- Denormals: the FPU keeps them and VMX flushes them. The recompiler tracks which mode a block is in and emits
  `enableFlushMode()`/`disableFlushMode()` (FPCR.FZ and FZ16 on ARM64) only on transitions; the mode resets to unknown
  at labels and after calls (`recompiler.cpp:423-434`, `:647`, `:699`; `xr:README.md:21`). This fits the PS3's default
  NJ = 1 (`rp:PPUThread.h:255`).
- Caveat if we borrow it: `HostToGuest` uses the x86 ordering and is shared with ARM64 (`ppc_context.h:218` against
  `:243`), so reading back the +∞ and −∞ rounding modes on ARM64 swaps them.

### 2.7 Xenon assumptions that do not carry over to the PS3

| XenonRecomp | Cell / PS3 (RPCS3 as reference) |
|---|---|
| Record forms compare the low 32 bits (`compare<int32_t>`, e.g. `recompiler.cpp:528-532`); `addic` takes CA from 32 bits (`:549-553`); `bdnz` tests `ctr.u32` (`:650-668`) | 64-bit mode: CR0 from the 64-bit result, 64-bit carry, full 64-bit CTR (`rp:PPUInterpreter.cpp:3157-3169`, `:3198-3215`) |
| `dcbz` zeroes 32 bytes (`:815-820`) | 128-byte cache line |
| `sync`, `lwsync` and `eieio` are no-ops (`:851`, `:1211`, `:1713`) | need real fences on weakly ordered ARM64 |
| `mftb` = `__rdtsc()`, or unscaled `cntvct_el0` on ARM64 (`:1262-1264`, `ppc_context.h:691-698`) | 79.8 MHz timebase |
| `lwarx`/`stwcx.` = compare-and-swap on the value (`:1189-1194`, `:1658-1665`) | 128-byte reservation shared with SPU GETLLAR/PUTLLC (plan §6) |
| VMX128, XEX loader, `.pdata`, MSVC switch patterns, ELF32-only loader | plain AltiVec plus Cell `lvlx`/`lvrx`; ELF64 and PRX; `.opd` and TOC; GCC switch patterns |

So XenonRecomp's emitter cannot be reused as is. Its structure, its VMX handling and its configuration model can.

### 2.8 Adopt

1. Unpacked CR (one byte per bit) and XER (one byte per flag), both in the context and in locals.
2. A weak-symbol override for every lifted function (`__imp__` implementation plus weak wrapper), to implement the
   manifest's function overrides.
3. Mid-asm hooks declared in the manifest: name, address, registers passed by reference, return or jump on true or
   false, before or after the instruction (`xr:README.md:207-238`, `recompiler.cpp:436-505`).
4. The TOML schema: explicit function boundaries, invalid-instruction skips, and switch tables as `{base, r, labels}`
   (`recompiler_config.cpp`). It fits `games/gta4/manifest.toml`.
5. A direct-indexed dispatch table (one load per indirect call), kept in host memory that guest code cannot write.
   Empty slots point at a stub that logs and aborts in dev builds and falls back to the interpreter in release builds.
6. Mode tracking for VMX flush-to-zero, if FPCR writes turn out cheap on the A19 **[S]**; otherwise flush per op in
   NEON.
7. The ABI-trusting local-variable modes, as opt-in optimizations once the oracle passes (§3).
8. Changed: a jump-table `switch` default must trap in dev builds instead of `__builtin_unreachable()`.

## 3. Verdicts on D-004 and D-009

### D-004 (full 16-byte reverse): confirmed

- It is XenonRecomp's convention (§2.2) and RPCS3's:
  - `vm::_ref<T>` returns `to_be_t<T>`, so `lvx` loads a byte-swapped `v128` (`rp:rpcs3/Emu/Memory/vm.h:302-304`,
    `rp:PPUInterpreter.cpp:4246-4256`);
  - `VPERM` indexes with `~c & 0x1f` (`rp:PPUInterpreter.cpp:1974-1996`);
  - the SPU preferred slot is `_u32[3]`, and `SHUFB` uses `~c & 0x1f` (`rp:SPUInterpreter.cpp:740`, `:1968-1979`).

  So RPCS3 semantics port over without re-indexing.
- ps3recomp is the counter-example: with raw big-endian registers every op that reads lane values needs a per-lane
  swap, and that is where its VMX bugs sit (§1.4).
- Suggested additions to the decision text:
  - **Extend the translation list to element-position ops:**
    - merges, packs and unpacks;
    - even/odd multiplies (`vmule*`/`vmulo*`: a guest even element is a host odd lane);
    - sums across lanes (`vsum*`, `vmsum*`) and splat-by-index;
    - `lvewx`/`stvewx`; `lvlx`, `lvrx`, `stvlx`, `stvrx`;
    - `mfvscr`/`mtvscr` (VSCR is guest word 3, which is host lane 0);
    - SPU `gb*`, `fsm*` and the `cbd` family, and every SPU preferred-slot scalar (host lane 3 for words).
  - **Whole-register byte shifts become native shifts.** The reversed register equals the guest's 128-bit big-endian
    value read as a little-endian integer. So `vslo`, `vsro`, `vsl`, `vsr` and SPU `shlqby`/`rotqby` map to `EXT` or
    `__int128` shifts, with a guest left shift being a host left shift. Derived from the definition.
  - **`vperm` and `shufb` become two NEON instructions:** `vqtbl2q_u8({vB, vA}, vbicq_u8(0x1F, vC))`. This is RPCS3's
    PANDN trick (`rp:PPUInterpreter.cpp:1985-1995`). Derived, not yet tested.
  - **Check the codegen of the 128-bit accessors.** `ps_ld128_rev` and `ps_st128_rev` in
    `runtime/core/include/pspocket/core/be.h` are byte loops; confirm they compile to one load plus `REV64`/`EXT` (or
    `TBL`) **[S]**.

### D-009 (signature): shape confirmed, two changes suggested

The shape matches XenonRecomp: context and base are both arguments, so the base stays in a register
(`xr:ppc_context.h:27`, `:105`). Differences:

1. **Drop `restrict` on `base`.**
   - XenonRecomp does not restrict `base`, and it makes guest scalar accesses `volatile`. D-009 restricts `base`, and
     today's `ps_read*`/`ps_write*` are a plain `memcpy` plus a byte swap (`be.h`).
   - Lifted code polls guest memory that other host threads write: SPU completion flags, `cellSync` primitives, RSX
     labels, the FIFO get pointer.
   - Without volatile or atomic accesses, clang may hoist or merge those loads; in C11 terms that is a data race, so
     undefined behaviour.
   - With `restrict`, any access to guest memory during the call that does not go through a pointer derived from
     `base` is also undefined behaviour. HLE code and other threads make exactly those accesses. Whether clang
     exploits this depends on escape analysis, so failures would be intermittent and change with the optimization
     level **[S]**.
   - Suggest: no `restrict` on `base`, and guest accessors in lifted code that the compiler cannot drop or merge
     (`volatile` as in XenonRecomp, or relaxed atomics where the access is aligned). Measure the cost on the device
     before relaxing either.
   - Keep `restrict` on `ctx` only if nothing reaches a thread's context through another pointer during a lifted call,
     for example a thread-local "current context" pointer.
2. **Add an alignment assumption on `base`,** as XenonRecomp's `PPC_FUNC_PROLOGUE` does (`ppc_context.h:32`), so that
   16-byte vector accesses at `base + (ea & ~15)` compile to aligned loads.

On register caching: D-009 loads everything into locals and spills around calls. That is ABI-agnostic and safe, but
it pays for stores before every call. XenonRecomp's speed came from trusting the ABI instead (§2.3). Keep D-009's
scheme as the default, and add XenonRecomp-style per-register-class options, controlled by the manifest, once the
interpreter oracle exists.

Naming (`f_%08X` against `sub_%X`) does not matter. Adding the `__imp__` plus weak-wrapper pattern costs nothing.

## 4. Recommendation for plan §14 "Toolchain"

**Write our own**, as the plan assumes. Reasons:

1. ps3recomp's correctness model (silent defaults, heuristic patching, no oracle) conflicts with rules 3 to 6. It
   would have to be removed, not extended.
2. Its register layouts contradict D-004, and its VMX code has open lane bugs.
3. Its hot paths (out-of-line instrumented accessors, no register locals, thread-local trampolines, scalar VMX loops)
   would have to be rewritten to fit a phone's frame budget, and that rewrite is the lifter.
4. It does not run any game on macOS, its Metal backend has not rendered a game, and it has no iOS support.
5. Legal hygiene: Sony firmware and keys are in the tree, most recent commits are AI-assisted and PRs merge without
   recorded review, and the README makes claims the code does not support.
6. XenonRecomp is the better design reference, but it encodes Xenon and x86 semantics (§2.7). It is a reference too,
   not a base.

What to take:

- From XenonRecomp (MIT, with attribution): the VMX tables and emission patterns, and the config, hook and override
  mechanisms.
- From ps3recomp (MIT, with attribution, each item re-checked against RPCS3): the pitfall catalogue and the synthetic
  jump-table vectors.

**Open item: the run on the GTA IV ELF.** It is not needed for this decision **[S]**: the blockers are architectural.
If a data point is still wanted, such as how many switch tables the GCC pattern matcher recovers, run ps3recomp's
lifter on the decrypted EBOOT inside a disposable VM, never on the host or in this repo, and count the markers with
`pr:tools/lift_audit.py`. That waits for a dump in any case (D-002).

Proposed entries for `docs/decisions.md`:

- Toolchain: our own C++20 lifter, with XenonRecomp as the reference; ps3recomp is not reused (this report).
- D-009 amendment: `base` not restrict-qualified, non-elidable guest accessors in lifted code, alignment assumption on
  `base`.
- CR and XER representation: unpacked bytes.
