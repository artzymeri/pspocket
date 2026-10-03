# Decisions

Every design decision or change, newest last. Format: ID, date, decision, why, consequences.
Open decisions from plan section 14 are tracked at the bottom.

## D-001 · 2026-10-03 · Licence: GPLv2

The plan's recommendation. The repo is private, so GPLv2 costs nothing, and it allows reusing RPCS3 code
(lv2 semantics, the RSX shader decompilers, the guest memory map) instead of clean-rooming it.
Consequences: `LICENSE` is GPLv2. Code taken or adapted from RPCS3 keeps a comment naming the source file.
Revisit only if the project ever needs a permissive licence (it would then need a clean-room rewrite of those parts).

## D-002 · 2026-10-03 · Game version: GTA IV BLES00229 (EU) with update 1.08 (tentative)

Tentative target, chosen because the `ps3-ios-engine` project worked with BLES00229 and the 1.08 update package
(`EP1017-BLES00229_00-GTAIVUPDATEV0108-A0108-V0100-PE.pkg`) is on the Mac. **The user has no GTA IV dump at the
moment**, so Phase 0's inventory and every GTA IV-specific step wait until there is a dump of a copy the user owns;
confirm region and version against that disc then. The recompiled EBOOT would be the one from update 1.08, which
replaces the disc's. Until then the toolchain and runtime are exercised with synthetic programs and homebrew, which
is what Phases 1–4 do first anyway. The manifest (`games/gta4/manifest.toml`) records hashes once the files exist.

## D-003 · 2026-10-03 · Host and languages

Toolchain and runtime: C++20, built with CMake + Ninja on macOS (Apple Silicon); Linux supported for the toolchain.
Generated code: C11 with `<arm_neon.h>` where needed, compiled by Apple clang. App shell: Swift/SwiftUI.

## D-004 · 2026-10-03 · 128-bit lane order: full reverse

VMX (PPU) and SPU registers are held in host order as a full 16-byte reverse of the guest's big-endian bytes:
host byte `j` = guest byte `15 - j`. So guest element `i` of an `N`-element vector is host lane `N - 1 - i`, already
in host byte order, and element-wise arithmetic maps straight to NEON. Loads and stores (`lvx`, `stvx`, `lqd`, `stqd`)
are a 16-byte aligned access plus one byte reverse (`ps_ld128_rev` / `ps_st128_rev` in `runtime/core/include/pspocket/core/be.h`).
Byte-indexed operations (`vperm`, `lvsl`, `lvsr`, `vsldoi`, SPU `shufb`, the SPU preferred slot) must translate guest
byte index `b` to host byte `15 - b`. Same convention as XenonRecomp and RPCS3's `v128` (confirmed by the Phase 0
evaluation, `docs/phase0-recomp-evaluation.md` §3).

Element-position operations need the same translation: merges, packs and unpacks; even/odd multiplies (a guest even
element is a host odd lane); sums across lanes and splat-by-index; `lvewx`/`stvewx`, `lvlx`/`lvrx`/`stvlx`/`stvrx`;
`mfvscr`/`mtvscr` (VSCR is guest word 3 = host lane 0); SPU `gb*`, `fsm*`, the `cbd` family and every preferred-slot
scalar (host lane 3 for words). Whole-register byte shifts map to native shifts (the reversed register is the guest's
128-bit value as a little-endian integer). `vperm`/`shufb` should become `vqtbl2q_u8({vB, vA}, vbicq_u8(0x1F, vC))`
(RPCS3's PANDN trick), to be verified by the fuzz tests.

## D-005 · 2026-10-03 · std::format instead of fmt

Apple clang 17's libc++ ships `std::format` (iOS 17 / macOS 14 deployment targets are above its availability floor),
which removes one dependency from the plan's list.

## D-006 · 2026-10-03 · Tests: Catch2 v3.16.0, vendored

The amalgamated build (`third_party/catch2/`, BSL-1.0) needs no network at configure time and no package manager.
Each component has a test executable registered with CTest through `pspocket_add_test()` (`tests/CMakeLists.txt`).

## D-007 · 2026-10-03 · iOS builds the runtime through XcodeGen

`app/ios/project.yml` defines a static-library target compiled from the same `runtime/` sources the CMake host build
uses, and the app links it. One `xcodebuild` builds and signs everything; no separate iOS CMake toolchain to keep in
sync. The host CMake build stays the source of truth for tests.

## D-008 · 2026-10-03 · Minimal iOS test host pulled forward to Phase 0

The plan brings a minimal iOS test host in at Phase 4. A smaller one (self-test probes, a Metal view with frame
times, no game) lands now, because the riskiest platform assumptions can only be checked on the device: signing with
the memory entitlements, reserving 4 GB of guest address space, fault-based write tracking, and the real speed of
interpreted versus lifted PPU code on the A19 Pro. Each later phase adds its probes.

## D-009 · 2026-10-03 · Lifted function signature (amended by D-013)

`void f_XXXXXXXX(PPUContext* restrict ctx, uint8_t* base)` where `XXXXXXXX` is the guest entry address in hex. The
guest base is an argument (as in XenonRecomp) so it stays in a register through call chains instead of being reloaded
from the context. Context registers are loaded into locals at entry and spilled around calls and syscalls.
XenonRecomp-style ABI-trusting register modes may be added later as manifest-controlled options, once the oracle passes.

## D-010 · 2026-10-03 · Host page granularity

Apple silicon uses 16 KB pages (macOS and iOS). Guest memory is committed and protected at 16 KB granularity; guest
4 KB page semantics that need finer control (stack guard pages, 4 KB write tracking) are handled in software where a
game actually depends on them.

## D-011 · 2026-10-03 · Primary test device and signing

iPhone 17 Pro Max (A19 Pro, iOS 26), team M7XJA39ZV6 (paid, already proven with the two memory entitlements in
`ps3-ios-engine`). Bundle IDs live under `com.artzymeri.pspocket`.

## D-012 · 2026-10-03 · Toolchain: our own lifter; ps3recomp is not reused

From the Phase 0 evaluation (`docs/phase0-recomp-evaluation.md`). ps3recomp (MIT) fails silently where our rules
demand loud failures (unknown instructions become no-ops, unresolved imports return CELL_OK), has no interpreter
oracle, uses a vector layout that contradicts D-004 with open lane bugs, runs no game on macOS and has no iOS support.
It also ships decrypted Sony firmware and keys, so nothing is copied from it; at most its pitfall list and synthetic
jump-table cases are used as test ideas, re-checked against RPCS3. XenonRecomp (MIT) is the design reference for the
emitter, VMX handling, config/hook/override mechanisms, but it encodes Xenon assumptions (32-bit compares, 32-byte
`dcbz`, no-op fences, unscaled `mftb`) that do not hold on the Cell. RPCS3 stays the authority for semantics.

## D-013 · 2026-10-03 · Guest accessors are volatile; `base` is not restrict-qualified

Guest code polls memory that other host threads write (SPU completion flags, cellSync, RSX labels, the FIFO get
pointer). `ps_readN`/`ps_writeN` in `be.h` are therefore volatile accesses through byte-aligned types: the compiler
cannot hoist, merge or drop them, unaligned guest addresses stay well-defined, and on ARM64 each is still one
`LDR`/`STR` (+ `REV`) with `base + uxtw(addr)` folded into the addressing mode. `restrict` is dropped from `base` in
the lifted signature, because HLE code and other threads legitimately touch guest memory during a lifted call. Lifted
prologues assume `base` is 64 KB-aligned. Host-pointer accessors (`ps_ldN`/`ps_stN`) stay plain. Cost on the M4:
scalar big-endian loads 12 GB/s (the earlier 50–60 GB/s came from auto-vectorization lifted code can't use anyway).

## D-014 · 2026-10-03 · iOS build and run flow

`scripts/ios.sh` is the entry point (`gen`, `build [--sim]`, `install`, `run`, `report`, `all`). It builds the host
CMake tree first, so build-time generated sources exist, then runs XcodeGen and `xcodebuild`. A runtime component joins
the iOS build when its `CMakeLists.txt` exists (as `pspocket_add_component` does on the host); every `.c`/`.cpp` in its
`src/` and in `<host build>/generated/selftest/` is compiled, so host-only code never goes in a runtime `src/`. The
runtime library is force-loaded into the app so self-registering code isn't dropped. Release everywhere, including the
Xcode scheme, because the probes' numbers only mean something optimised. Device signing uses automatic provisioning
(`-allowProvisioningUpdates`), which needs Xcode signed in to team M7XJA39ZV6.

## D-015 · 2026-10-03 · Self-test threading on iOS

Probes run on a dedicated thread with an 8 MB stack at userInitiated QoS, matching the macOS CLI's main thread. GCD
threads have 512 KB, and lifted code nests host calls as deep as the guest nests guest calls. The same applies later to
PPU threads: their host stacks must be sized for guest call depth.

## D-016 · 2026-10-03 · Output preview and frame pacing

The test host renders a 1152×640 bgra8 target, upscales it with MetalFX spatial (perceptual) to an aspect-fit texture,
and composites it letterboxed; a CADisplayLink at 30/30/30 drives frames, presented with
`present(atTime: link.targetTimestamp)`, and frame intervals come from `presentedTime`. MetalFX falls back to a
bilinear blit where unavailable (the simulator), labelled on screen. This is the shape the RSX backend's output stage
will take (plan section 7).

## D-017 · 2026-10-03 · iOS logging

`pspocket::log` goes to os_log with subsystem `com.artzymeri.pspocket` and the log channel as category; Info/Warn map to
default, Error to error, Fatal to fault. The test host also mirrors lines to stdout (visible through
`devicectl ... --console`) and into the self-test report.

## D-018 · 2026-10-03 · Build the game-independent engine before the Phase 0 inventory

The user asked for the whole engine to be built now rather than waiting for a GTA IV dump. Work that every PS3 title
needs proceeds ahead of the Phase 0 gate: PPU VMX, the SPU interpreter and lifter, the shared reservation system,
the core lv2 kernel (threads, synchronization, memory, time, filesystem, events), and the RSX command parser and Metal
backend. Rule 4 still holds for everything title-specific: firmware modules beyond what homebrew and every game use,
SPU program replacements, patches, and the GTA IV bring-up wait for the inventory. Phase gates remain the measure of
done; they are no longer the order in which work starts.

## D-019 · 2026-10-03 · NID and syscall names come from RPCS3, by script

`recomp/common/scripts/harvest_nids.py <rpcs3-dir>` regenerates `recomp/common/data/nids.txt` (6,625 names in 148
modules) and `data/syscalls.txt` (634 syscalls) from RPCS3's registration macros and syscall table. They are API
names, so they are committed and embedded into `pspocket::recomp_common` at build time; more names files can be loaded
at runtime (`ps3inspect --names`). Rerun after RPCS3 updates; `--check` detects drift. The runtime gets its own
committed syscall-name table generated from the same file (it can't depend on recomp/).

## D-020 · 2026-10-03 · Toolchain parsers never abort on bad input

ELF, SELF, PRX, TOML and manifest parsing use bounds-checked reads and return errors as values; `PS_CHECK` is only for
programming mistakes. Robustness tests (truncations, 60k random mutations) run under ASan and UBSan. SELF handling reads
the plaintext headers only and reports the licence type; it never decrypts. A PRX is viewed statically with each
segment at its `p_vaddr` and relocations applied the way RPCS3's `ppu_load_prx` does.

## D-021 · 2026-10-03 · Manifest format, SPU program identity, Phase 0 outputs

- `games/<title>/manifest.toml` is a strict TOML subset: unknown keys are errors, every patch needs a `why`,
  overlapping patches are rejected, hashes are optional in drafts and required for builds, paths are relative to a
  dump laid out like the PS3 filesystem (`dev_bdvd/`, `dev_hdd0/`).
- SPU programs are keyed by the SHA-1 of their ELF image bytes. RPCS3's `SPU-`/`PPU-` hashes are printed alongside so
  results can be matched against RPCS3 logs. The key for code loaded at runtime (overlays, SPURS jobs) is decided in
  Phase 3.
- Phase 0 outputs (`ps3inspect --json`, traces) live in `inventory/<title>/`, which is gitignored; only the decisions
  and the re-estimate go into docs. `ps3inspect` exits 0 (ok), 1 (usage or I/O error), 2 (bad input), 3 (encrypted
  SELF).

## D-022 · 2026-10-03 · Phase 0 runs on GTA V (BLES01807), the dump on hand

The user's dump is GTA V, not GTA IV: BLES01807 (EU), disc version 01.03, app 01.00, 18.74 GB, decrypted at disc
level (its region table still marks 18.47 GB encrypted; the EBOOT reads as a SELF, so the content isn't). It lives on
the phone in the app's Documents; `-extract-iso` copied out PS3_DISC.SFB, PARAM.SFO and EBOOT.BIN (14.6 MB, a disc
SELF with no NPDRM header: no RAP needed). GTA V ships no separate PRX modules. The Phase 0 inventory runs on this
EBOOT once RPCS3 has decrypted it; whether the first title becomes GTA V instead of GTA IV (D-002) is decided from
that inventory. GTA V is the heavier game (more SPU work, larger engine), so the go/no-go matters more, not less.

## D-023 · 2026-10-03 · PPU interpreter and lifter conventions

- **Registers in lifted code:** CR is `uint8_t cr[32]` (one byte per bit) and XER is separate so/ov/ca/bc bytes, both
  in the context and in locals; FPRs are doubles. A lifted function loads every register it references at entry,
  spills the ones it wrote before calls and returns, and reloads after calls; nothing relies on the ABI.
- **FPSCR:** the architected 32-bit register. Writes follow the architecture (FEX/VX recomputed, `mtfsb1` on an
  exception bit sets FX, `mcrfs` clears what it copies); `fcmpu`/`fcmpo` set FPCC; arithmetic sets no FPRF or
  exception bits and rounds to nearest (as RPCS3 by default); only `fctiw`/`fctid` honour RN. Rc=1 FP forms set
  CR1 = FX FEX VX OX as the architecture says (RPCS3 differs). The interpreter warns once if a guest sets RN ≠ 0.
- **Deviations from RPCS3, on purpose:** D-form with rA = 0 uses a literal 0; `fctiw` honours RN; the FPSCR
  instructions are complete. `stfs` rounds like RPCS3 (hardware truncates).
- **Dispatch:** one 8-byte slot per 4-byte code address over the registered range; empty slots point at a miss stub,
  so an indirect call is a bounds check plus one load. Miss policy: abort without NDEBUG, interpret with NDEBUG
  (configurable). One range of up to 256 MB today; PRX modules will need more ranges.
- **Interop contract:** a lifted function behaves as "run from the entry until `blr`", then execution continues at
  LR. An interpreter fallback stops when cia equals the entry LR and r1 equals the entry SP.
- **Jump tables:** a `switch` on the computed CTR value whose `default` traps (`ps_ppu_jump_table_miss`).
- **Fuzzing:** whole-context comparison, bitwise, except that FP-arithmetic forms accept any NaN for any NaN.
- **Names:** lifted functions are `<prefix>f_XXXXXXXX` (upper-case hex) with a per-image `<name>_register()`.
- **Build flags:** lifted code is never compiled with `-ffast-math`/`-Ofast`; `lifted.h` disables FP contraction.
- **Open:** VMX is decoded but not executed; reservations still use a single-thread model in the context
  (`ps_ppu_lwarx`/`stwcx` in runtime/ppu/src/runtime.cpp) until they move onto `reservation.h`.

## D-024 · 2026-10-03 · Phase 0 result for GTA V: go, SPURS by LLE (proposed)

From `docs/phase0-gta5.md`: 85,342 functions / 6.09 M PPU instructions, every scalar instruction already supported,
448 k VMX instructions over 98 mnemonics (in progress), 362 firmware imports all named (184 are online/PSN/trophy
non-goals, stubbed as "offline"), 8 embedded SPURS tasks (494 KB), and job chains that only a runtime trace will
reveal. **Go**, with SPU throughput on the phone as the decisive risk and a checkpoint at the first lifted physics task.
**SPURS: LLE proposed** — recompile Sony's libsre (PPU + SPU) from the user's own firmware file like game code,
because GTA V uses tasksets and job chains (RPCS3 relies on LLE for these); HLE stays the fallback. Pending the
user's firmware file. Raw inventory outputs stay in `inventory/`; the aggregate report goes to docs (amends D-021).
The analyzer recognised no jump tables in this EBOOT (SNC rather than GCC patterns, to investigate).

## D-025 · 2026-10-03 · First title: GTA V (BLES01807), pending the user's confirmation

The user owns a GTA V dump and no GTA IV dump, and the inventory says go. `games/gta5/manifest.toml` is the active
manifest; `games/gta4/` stays as a draft. Revert to GTA IV only if the user gets that dump and prefers it.

## Open decisions (plan section 14)

- [x] Licence → D-001.
- [~] Game version → D-025: GTA V BLES01807 (pending the user's confirmation; GTA IV in D-002 as a draft).
- [x] Toolchain → D-012 (write our own).
- [~] SPURS → D-024: LLE proposed (needs the user's firmware file); HLE fallback.
- [~] Target devices: primary test device is D-011; minimum chip and the iPad question are still open.
- [ ] Input: controller required, or a polished touch overlay as well.
- [ ] ISO handling: extract assets, or read directly from the ISO.
