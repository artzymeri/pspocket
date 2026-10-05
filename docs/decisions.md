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

## D-025 · 2026-10-03 · First title: GTA V (BLES01807), confirmed by the user

The user owns a GTA V dump and no GTA IV dump, the inventory says go, and the user confirmed the switch.
`games/gta5/manifest.toml` is the active manifest; `games/gta4/` stays as a draft. Firmware for SPURS LLE (D-024):
the user approved using Sony's official PS3UPDAT.PUP; it is placed by the user (this session's tooling may not
fetch it) under `inventory/firmware/`, outside git.

## D-026 · 2026-10-03 · Clang only; the tree builds and tests on Linux

Cloud sessions run on Linux, so the host build (toolchain, runtime, tests) must work there too, not only on the Mac.
The tree now builds and passes ctest on Linux with LLVM Clang: `sys/sysctl.h` is included on Apple only, the QoS
probe's helper is unused elsewhere, and 128-bit integers go through `s128`/`u128` in `pspocket/core/types.hpp`
(`__extension__`, so `-Wpedantic` stays quiet). GCC is rejected at configure time: the interpreter relies on
`#pragma clang fp contract(off)` for bit-exact FP (GCC would silently fuse multiply-adds), and GCC 13's
`-Wnull-dereference` fires inside libstdc++. Apple Clang remains the reference compiler.

## D-027 · 2026-10-03 · AArch64 tests run under qemu on x86 Linux; host-independent stack layout

The lifted VMX helpers (`pspocket/ppu/vmx.h`) are NEON and the SPU FP environment is AArch64 FPCR (D-003), so an
x86 host can't run those paths natively. `cmake/toolchains/linux-aarch64-qemu.cmake` cross-builds the tree with
Clang for aarch64-linux-gnu and runs the build-time generators and ctest through qemu-user; the whole suite passes
there, VMX fuzzing included. x86 builds stay supported for everything else: the PPU fuzz test lowers its form-count
floor when the cases were generated with `--no-vmx`, and the SPU FP environment adds MXCSR FTZ/DAZ to the rounding
mode on x86-64 (default-NaN behaviour remains the host's; best effort).

lv2 thread stacks are laid out in 16 KB units on every host (size rounded up and a 16 KB guard below), not in host
pages: guest-visible stack addresses must not differ between a 4 KB-page host and the phone.

## D-028 · 2026-10-03 · PPU VMX: RPCS3 accuracy model, NEON lifted code, scalar oracle

Recorded after the fact for the agent work merged in 8e99d87; full detail in `pspocket/ppu/vmx.h`.
All VMX instructions execute in the interpreter (`runtime/ppu/src/interpreter_vmx.cpp`, scalar, per element) and
lift to NEON (`vmx.h`, one static inline function per instruction, `uint32x4_t` locals in D-004 order); the
differential fuzz compares them bit for bit, NaN payloads included. Semantics are RPCS3's with its accuracy options
on: VSCR[NJ] flushes denormals (inputs and the results RPCS3 flushes), NaN results are the first NaN operand
(vA, vB order, quieted) else 0x7fc00000, VSCR[SAT] is sticky (a vector accumulator in lifted code). vrefp,
vrsqrtefp, vexptefp and vlogefp are correctly rounded (host libm), not the hardware estimates. mfvscr/mtvscr use
guest word 3 as the architecture does (RPCS3's interpreter uses word 0). Lifted VMX is AArch64-only (D-003); x86
hosts fuzz it under qemu (D-027).

## D-029 · 2026-10-03 · SPU: interpreter, lifter, channels and MFC; FP model in the host FP environment

Detail in `pspocket/spu/fp.h`, `context.h`, `runtime.h` and `recomp/spu/.../lifter.hpp`. SPU code runs with the
host FPCR set to FZ + round-toward-zero + DN, entered once per SPU thread, so single-precision arithmetic is native.
Two single-precision models, per context: APPROXIMATE (default; RPCS3's default xfloat: IEEE single in that
environment, with RPCS3's fm/fma/fs fixups) and ACCURATE (extended range in double precision). Double precision
rounds to nearest inside one FPCR window per instruction. frest/frsqest/fi are RPCS3's tables, bit-exact. Lifted
programs are keyed by a hash of their LS code range, entered at any block start, and fall back to the interpreter
(logging the address) for unknown targets. Channels, mailboxes, signals, events, the decrementer and MFC DMA/list
commands live in the runtime; contexts without a host thread stop with PS_SPU_STOP_BLOCKED instead of blocking.

## D-030 · 2026-10-03 · One reservation system for PPU and SPU: seqlocked 128-byte line versions

Detail in `runtime/core/src/reservation.cpp` (contract: `pspocket/core/reservation.h`). 32768 version slots, one per
128-byte host cache line, hashed from the guest line index. A slot is a seqlock and a stripe lock at once;
conditional stores compare the whole line against the snapshot and write changed 8-byte words with CAS, so plain
stores (which take no lock) are never lost. Waits with timeouts use parked mutex/condvar buckets counted per slot,
so an uncontended store costs one extra load.

## D-031 · 2026-10-03 · lv2: host threads, no scheduler emulation, VFS over host directories

Detail in `pspocket/lv2/kernel.hpp` and the source headers in `runtime/lv2/src/`. Every PPU thread is a host thread
(8 MB stack, D-015) running lifted code with interpreter fallback; blocking syscalls block it; priorities only order
SYS_SYNC_PRIORITY wait queues and choose host QoS. Thread exit unwinds through lifted code with a C++ exception, so
lifted code must keep unwind tables. Memory follows RPCS3's area layout (256 MB user areas, stacks in
0xD0000000..0xE0000000, D-027 for their granularity). sys_fs maps mount points to host directories with normalised
paths (no escape from a mount), no NPDRM, case-sensitive like iOS. Not yet: interrupt-thread establishment,
sys_mmapper access-right changes and page-fault notification.

## D-032 · 2026-10-03 · RSX: portable front end, Metal backend, write tracking by protection or hash

Detail in `pspocket/rsx/rsx.hpp`, `shader/ir.hpp`, `write_tracker.hpp`. The FIFO parser, method table, register
state, shader microcode decoding (VP/FP to an IR, then MSL) and texture layouts are portable C++ tested on any host;
the Metal backend (`runtime/rsx/metal/`, Objective-C++ with ARC) builds on Apple platforms only. Semaphores, labels
and reports are written in command order. Written guest memory behind GPU copies is detected either by page
protection (the plan's design; 3.7 µs per fault on the A19 Pro) or, until the platform fault handler is installed,
by content hashes.

## D-033 · 2026-10-03 · HLE linkage and the process loader; pspocket-boot for Phase 2 bring-up

`runtime/hle` holds the firmware modules implemented on the host and the per-process linkage; `runtime/loader` turns
a decrypted ELF into a running lv2 process; `pspocket-boot` runs it under the interpreter on any host.
- **Imports** link the way RPCS3 links HLE functions (PPUModule.cpp): one 8-byte entry per function in a main-area
  block, `{func = entry + 4, toc = 0}`, the zero TOC word doubling as the stub instruction. The stub address is
  registered in the PPU dispatch table with a trampoline that identifies the import from `ctx->cia`, so lifted code
  and the interpreter reach HLE through the ordinary indirect-call path, with no special instruction. Executing the
  stub word directly is an illegal instruction (fails loudly). Unimplemented functions log
  `UNIMPLEMENTED module::function (NID)` with the caller and r3..r6 and return CELL_ENOSYS, or abort with
  `--stop-at-first`; imported variables are logged and left unpatched. Reference tables (attribute 0x2000) are
  patched with RPCS3's ppu_patch_refs types 1/4/6/57.
- **Registration** is explicit (`register_<module>()` listed in `runtime/hle/src/modules.cpp`), not static
  initialisers, which a static-library link drops. NIDs are written in the source and checked against `nid_of(name)`
  by the tests, so the runtime does not need the toolchain's SHA-1 and NID database.
- **Process start** follows RPCS3 ppu_load_exec with liblv2 in HLE mode (no firmware needed): segments at p_vaddr in
  the main area (256 MB, or 512 MB for SDK < 2.0), main thread with sys_process_param's priority and decoded stack
  size, detached; `sysPrxForUser::sys_initialize_tls` runs on the main thread before the entry (TLS blocks of
  0x30 + p_memsz in a 256 KB main-area region, r13 = block + 0x7030); argv/envp as u64 pointer arrays at the stack
  top; entry registers r3..r12 as RPCS3 sets them.
- **Allocation in the main area** (`hle::MainMemory`, RPCS3 vm::alloc in vm::main) is first fit at 64 KB alignment
  in whole host pages, after the loader reserves the executable's segments.
- Not yet: PRX loading (the game's own modules, LLE firmware libraries), write protection of read-only segments,
  charging the segments to the default memory container. The loader uses recomp/common's ELF and module parsers;
  the iOS app will need them on device too (the EBOOT comes from the user's disc), to be wired in when the app boots
  a game.

## D-034 · 2026-10-03 · Bring-up loop: boot log → HLE functions; guest faults reported, not fatal-silent

GTA V's first boot (docs/runs/004) set the loop for Phase 2: run `pspocket-boot`, implement what the log names, run
again. Decisions from the first round:
- **sysPrxForUser lwmutex** is RPCS3's user-space protocol (sys_lwmutex_.cpp without its `hle_lwmutex` option):
  atomics on the guest's big-endian owner/waiter word, the kernel sleep queue (`_sys_lwmutex_*`, already in lv2) only
  on contention, the `reserved` owner value handing the lock to the woken waiter. The whole set (create, destroy, lock,
  trylock, unlock) lands together because a created lwmutex is only used through it. HLE functions get guest
  scratch variables on the caller's stack (RPCS3 vm::var: r1 moves below them for their lifetime).
- **null_func syscalls**: lv2 binds a syscall that RPCS3 answers with `null_func` (CELL_OK, logged) only when a game
  is seen calling it, with RPCS3's table comment as its description (`SyscallTable::bind_null`). First: 988.
- **Guest faults** in `pspocket-boot` print the guest address, the PPU thread and its cia/lr/r1/r2/r3 and exit with
  128 + signal (a debugger still stops at the fault first). The runtime-wide handler (write tracking, release
  fallback) is the RSX write tracker's job later; this one is for bring-up.
- Unimplemented HLE calls are logged like unimplemented syscalls: the first 8 calls, then every 1024th.

## D-035 · 2026-10-03 · cellSysmodule is bookkeeping; HLE modules keep per-process state

Run 005. Every imported firmware function is linked to HLE (or the UNIMPLEMENTED stub) at load time, so
`cellSysmoduleLoadModule` does not load anything: it keeps RPCS3's per-module load counts, loads dependencies first
and unloads them last, refuses 0x58, and counts lv2's default modules (SYSUTIL, GCM_SYS, AUDIO, IO, SPURS, and FS from
SDK 3.3) as loaded from the start, exactly as RPCS3's cellSysmodule.cpp does minus its sys_prx calls. The module table
is generated from RPCS3's MODULE_INFOS. Internal IDs (0xF000+) return CELL_SYSMODULE_ERROR_UNKNOWN with an error log
until a game uses one. Module state lives in `HleProcess::state<T>()` (RPCS3's g_fxo), so tests and future
restarts get a fresh copy per process.

## D-036 · 2026-10-03 · Goal: any PS3 game, one Mac-side build per game; GTA V stays the driving title

The user widened the goal from one title to any PS3 disc image. What that means under iOS's rules:
- **Still static recompilation, one build per game.** iOS gives apps no JIT, so a game's PPU and SPU code is lifted
  and compiled on the Mac (disc image in, signed app out). The interpreter remains the fallback for code the build
  missed, ~12× slower (run 003), too slow to be the main path. A phone-only "drop an ISO in" flow isn't possible.
- **The runtime is shared by every game**: lv2, HLE modules, SPU, RSX→Metal. Rule 6 already keeps it title-free; HLE
  and syscall coverage grows with each game's boot log, as RPCS3's did (rule 4 applies per title).
- **GTA V stays the first and driving title**; its gaps are worked first. Other titles follow once it boots.
- Consequence for SPURS (amends D-024): a native SPURS has to cover every SPURS feature any game uses, and RPCS3 has
  no reference for two of them (job chains: `spursJobChainEntry` is a stub; SPURS queues: UNIMPLEMENTED). See the
  open question below.

## D-037 · 2026-10-03 · SPURS is native: our own C++ implementation, no Sony code in the app

The user chose a native SPURS (supersedes the LLE proposal of D-024 and the baseline idea in D-036). cellSpurs is an
HLE module like the others: the PPU API on the host, and a host scheduler that runs each workload's SPU code (task
ELFs, job binaries; lifted like any SPU program) on SPU contexts, without Sony's SPU-side SPURS kernel. Guest-visible
structures (CellSpurs, attributes, tasksets, event flags, ...) keep RPCS3's layouts (cellSpurs.h), because games and
their SPU code read them. Reference: RPCS3 cellSpurs.cpp / cellSpursSpu.cpp where they implement a feature. Where they
don't (job chains: `spursJobChainEntry` is empty; SPURS queues: UNIMPLEMENTED), the reference is Sony's libsre from the
user's firmware file, read on the Mac to learn the behaviour and used as a test oracle, never shipped or committed.
RPCS3's SPU side is a reference for structures only: its native task and kernel entry points are commented out and it
runs libsre's policy-module code even in HLE mode. Architecture and stages: docs/spurs-native.md. Stage 1 (attributes,
initialize, finalize) does not create the SPU thread group, handler threads, lv2 semaphores or the guest lwmutex/lwcond
that libsre's kernel uses; those CellSpurs fields stay 0.

## D-038 · 2026-10-03 · sys_vm without paging (as RPCS3)

Run 006. `sys_vm_memory_map` reserves a 256 MB-aligned area, commits the whole virtual size at once and charges only
the physical size to the memory container; nothing is paged, so lock/unlock/touch/flush/invalidate/store/sync only
validate their range, `sys_vm_test` reports every page on memory and the statistics show no paging. 256 MB of virtual
memory per process. Host memory is committed lazily by the OS, so an untouched virtual range costs nothing on iOS. If
a game's working set ever outgrows the phone, real paging (decommit on `sys_vm_store`/`invalidate`) is the lever.

## D-039 · 2026-10-03 · cellSheap is native with assumed semantics; only functions a log shows

Run 008. Sony's shared heap (cellSheap) lives in libsre and RPCS3 has no implementation (UNIMPLEMENTED stubs that
return CELL_OK), so there is no reference for its behaviour or its guest-memory layout. `cellSheapInitialize` keeps
the heap on the host and writes nothing into the guest range; `cellSheapAllocate` / `cellSheapFree` are left
unimplemented until a boot log shows them called, so their exact signatures come from the game's own calls rather
than a guess. To be checked against libsre once the firmware file is available (D-037). Also from run 008: SPURS
event flag initialize and workload shutdown follow RPCS3; natively no SPU runs a workload yet, so a shutdown makes it
removable at once and the completion hook (unused by GTA V so far) is logged, not called.

## D-040 · 2026-10-03 · SPURS SPU ports live in the native instance; event flags follow RPCS3 with two fixes

Run 011. On the PS3, attaching an lv2 event queue to SPURS (`cellSpursAttachLv2EventQueue`, and the queue an event
flag gets in `cellSpursEventFlagAttachLv2EventQueue`) connects it to an SPU port on every thread of SPURS' SPU thread
group. A native SPURS has no thread group, so each SPURS instance keeps its own port → queue map, with lv2's rules: a
given port 0..0x3F (0..0xF from SDK 1.8), or the lowest free one of 0x10..0x3F, and a port whose queue was destroyed
is free again. The native scheduler will send SPU-side events (an SPU task setting an event flag, a SPURS queue push)
through this map. Event flag set and wait run as one reservation operation on the flag's 128-byte line, so they stay
atomic against SPU code using GETLLAR/PUTLLC on it. Two departures from RPCS3, both bugs there: a blocked
`cellSpursEventFlagWait` returns the bits that woke it, and detaching a queue also disconnects its port. Waking SPU
tasks that wait on a flag needs the native taskset policy; until it exists that case stops with a fatal log instead of
being skipped.

## D-041 · 2026-10-03 · SPURS task creation: assumed LS pattern semantics, our own task attribute layout

Run 012. GTA V creates SPU tasks with `cellSpursTaskGetLoadableSegmentPattern`, `...GetReadOnlyAreaPattern`,
`...GenerateLsPattern`, `...GetContextSaveAreaSize`, `_cellSpursTaskAttributeInitialize` and
`cellSpursCreateTask[WithAttribute]`. RPCS3 implements only `cellSpursCreateTask`; the rest are stubs, so their
semantics are taken from what RPCS3's taskset code does with their results:

- An LS pattern has one bit per 2 KB block of local store, block 0 the top bit; the six blocks below 0x3000 belong to
  SPURS and may not be set (RPCS3 `_spurs::create_task`).
- The loadable-segment pattern is every block a PT_LOAD segment touches. The read-only pattern is only the blocks that
  read-only segments cover completely: when a task resumes, its read-only segments are reloaded from the ELF and the
  pattern's blocks are restored from the context (RPCS3 `spursTasksetLoadElf`), so a block shared with writable data
  has to be treated as writable.
- `GenerateLsPattern(start, size)` wants 2 KB alignment and a range inside 0x3000..0x40000.
- The context save area is 1 KB of execution context plus 2 KB per pattern block, the inverse of RPCS3's
  `(size - 0x400) >> 11`.

Every one of these logs "[native, assumed semantics]" so a boot log shows what the game asked for. They are checked
against libsre once the firmware reference is set up (D-037). `CellSpursTaskAttribute` is opaque (256 bytes), so
pspocket stores its own fields there. A created task is marked pending-ready and its taskset's workload is signalled,
but nothing runs it until the native taskset policy exists (stage 3b).

## D-042 · 2026-10-04 · RPCS3 runs Sony's libsre by default; HLE functions check the pointers they read

Run 013. RPCS3 does not use its own cellSpurs code for games by default: `g_prx_list` in
`rpcs3/Emu/Cell/lv2/sys_prx.cpp` marks `libsre.sprx` as not HLE, so libsysmodule loads the firmware's libsre and its
PPU code runs as it is. RPCS3's cellSpurs.cpp is therefore a partial, rarely exercised reference (it is how so many of
its task functions can be stubs). Consequences:

- libsre is the authority for the PPU side of SPURS as well, not only for job chains and queues (D-037). What was taken
  from RPCS3's HLE (initialize, workloads, tasksets, event flags, job chain attributes, D-040, D-041) gets checked
  against it once the firmware reference is set up.
- An HLE function checks a guest pointer before it reads it (`guest_readable`). A wrong guess about a firmware
  signature then gives a logged error, not a host crash: the run goes on and the log shows what the game passed.
- Those errors, unimplemented calls and `pspocket-boot`'s crash report show r3..r10, and the crash report and
  `log_call_site` disassemble the caller's last 16 instructions, so one run shows what the game put in each register.
- From run 013: a task's LS pattern is read only when the task has a context. The pattern only describes what the
  context keeps, and GTA V's first `_cellSpursTaskAttributeInitialize` passed 0x40 in one of those two registers.

## D-043 · 2026-10-04 · The game reads its disc image in place; boot.sh finds it or copies it from the iPhone

Run 014. GTA V's first file access (`cellFsStat` and `cellFsOpen` on `/dev_bdvd/...`) needs the disc. That settles
the open question of whether to extract the disc or read the ISO: **read it in place**. lv2 gets a read-only
`VfsImage` interface, and the loader puts runtime/disc's ISO 9660 reader behind it. `/dev_bdvd` mounts the image root
and `/app_home` mounts `PS3_GAME/USRDIR`, where RPCS3 puts `/app_home` for a game started from the drive. Every sys_fs
syscall reads image mounts with the results a read-only host directory gives. Lookups ignore case, as the reader does
on PS3 discs, and files over 4 GB read across their extents. Nothing is extracted, so the 18.7 GB GTA V image costs no
extra space, and the iOS app can use the same path on the copy it already holds.

On the Mac, `scripts/boot.sh` takes the first image that reads as a PS3 game disc (`pspocket-boot --probe-disc`),
searching in this order:
1. `$PSPOCKET_ISO`;
2. `inventory/<game>/`;
3. the Desktop and Downloads.

If none of those has one, it copies the image the app holds on the connected iPhone into `inventory/<game>/disc.iso`
once (`scripts/ios.sh pull-iso`, through devicectl). That image, like everything in `inventory/`, never goes into git.
The game's `/dev_hdd0` is `inventory/<game>/hdd0`, for game data and saves.

## D-044 · 2026-10-04 · cellFs over lv2, cellGame as RPCS3 without the delays, sceNp2Init offline

Run 014.

- **cellFs.** The 20 `cellFs` functions GTA V imports forward to lv2 as RPCS3's `cellFs.cpp` does. Read and write at
  an offset go through `sys_fs_fcntl` 0x8000000A/B with RPCS3's argument block, and allocate-area goes through
  0xE0000017. lv2 gains `sys_fs_fcntl`, `sys_fs_chmod` and `sys_fs_utime`. Deviation: an fcntl operation lv2 doesn't
  implement logs and returns `CELL_EINVAL`, where RPCS3 returns `CELL_OK` (rule 4).
- **cellGame.** It follows RPCS3's `content_permission`: a check opens a session and ContentPermit ends it. Game data
  is created in a temporary directory and moved into place with a PARAM.SFO written by a PSF writer that follows
  RPCS3's `save_object`. RPCS3's sleeps, which imitate the PS3's slow calls, are left out. The error dialog only logs,
  since there is no UI yet.
- **sceNp2Init.** It brings NP up with no user signed in, so the status reads offline.

## D-045 · 2026-10-04 · The RSX lives in lv2; cellGcmSys is HLE on top of it, as in RPCS3

Runs 015 and 016. GTA V's `_cellGcmInitBody` is the first graphics call, so the RSX front end (D-032) gets wired to a
game for the first time:

- **lv2 owns the RSX** (`runtime/lv2/src/rsx.cpp`, RPCS3 `sys_rsx.cpp`). `sys_rsx_memory_allocate` commits local memory
  at 0xC0000000. `sys_rsx_device_map` reserves the rsx_context area through the same first-free-256 MB allocator as
  user memory, as RPCS3's `vm::reserve_map` does (GTA V: 0x40000000). `sys_rsx_context_allocate` fills DMA control,
  driver info and reports as RPCS3 does, then creates and starts `rsx::Rsx`. The kernel config's `rsx_backend`
  factory supplies the backend; without one, the new `rsx::NullBackend` runs every command and draws nothing, which is
  what `pspocket-boot` uses on the Mac. Unhandled RSX methods abort (rule 5) unless `rsx_log_failures` is set.
- **These are C++ entry points for HLE, not bound syscalls.** No game has called syscalls 666-677 directly (rule 4).
- **cellGcmSys** (`runtime/hle/src/cellGcmSys.cpp`): the 20 functions GTA V imports, following RPCS3. The
  CellGcmContextData sits at the start of the device area, and the offset tables, display info, tiles and Z-cull
  arrays sit in the main area, all where RPCS3 puts them. Display buffers, tiles and Z-cull go through
  `sys_rsx_context_attribute` (0x104, 0x300, 0x301) the way the firmware's libgcm does. RPCS3's HLE writes them into
  its renderer directly instead.
- **Internal HLE functions.** `cellGcmCallback`, which the game reaches through the context when a 32 KB segment is
  full, has to have a guest address without being an import. A registry flag (`PS_HLE_INTERNAL`) makes the loader
  link such functions for every imported module before guest threads start, because the dispatch table can't grow
  later. HLE looks them up with `HleProcess::descriptor_of`.
- **HLE can call the guest** (`call_guest` in `guest.hpp`). It gives the callee a fresh frame below r1, because the ABI
  lets a callee write its caller's LR, TOC and parameter save slots, and a `StackVar` sits at r1.
- **Vblank.** `_gcm_intr_thread` is an lv2 PPU thread whose entry is an internal HLE function. Every 1/59.94 s it counts
  a vblank in the driver info (package 0xFED, RPCS3's vblank hack) and calls the game's vblank handler with head 1.
  Flip and user handlers come when a game imports their setters.

## D-046 · 2026-10-04 · The firmware reference is decrypted on the Mac by a local script, never committed

D-037 and D-042 need Sony's libsre as the reference for SPURS. RPCS3 isn't installed on the Mac, but its source is
(`../ps3-ios-engine/external/rpcs3`). A small script follows RPCS3's own code: the PUP reader (`Loader/PUP.cpp`, entry
0x300 is update_files.tar), the firmware installer (`main_window.cpp`: each `dev_flash_*` package is an SCE container
whose third section is a tar), and `SELFDecrypter` for the SPRX. It reads the keys from RPCS3's `key_vault` source at
run time. Script and outputs stay in `inventory/firmware/` (gitignored): the script in `tools/fw_unself.py`, the ELFs
in `elf/`. From firmware 4.92 we have `libsre.prx.elf` (cellSync, cellSheap, cellSpurs, cellDaisy, ...) with its two
embedded SPU kernels carved out, and `libspurs_jq.prx.elf`. libsre also carries the SPU policy modules as raw code
blocks, the taskset module at LS 0xA00. Reading it uses scratch disassemblers built on the repo's PPU and SPU
disassemblers. Nothing from the firmware, and no disassembly of it, goes into the repository. Code comments describe
the behaviour they follow ("as libsre").

## D-047 · 2026-10-04 · SPURS tasks, taskset shutdown and cellSheap follow libsre (fw 4.92)

Run 015 (D-046 made the check possible). Changes from D-039, D-041 and D-042:

- **`_cellSpursTaskAttributeInitialize(attr, revision, sdk, elf, saveConfig, argument)`** takes six arguments. The
  fifth is a `CellSpursTaskSaveConfig {u32 context; u32 contextSize; u32 lsPattern*}`. This explains run 013: Bink
  passed a stack save config and no argument, and garbage sat in r9/r10. The attribute now uses libsre's layout, which
  replaces D-041's own:

  | Offset | Field |
  |---|---|
  | 0x08 | ELF (u64) |
  | 0x10 | context (u64) |
  | 0x18 | size |
  | 0x20 | pattern |
  | 0x30 | argument |
  | 0x40 | exit code container |

  The attribute only needs to be 8-byte aligned.
- **Task creation** follows libsre's check order and now checks the ELF. Its ident must be `7F 45 4C 46 01 02 01 00`,
  with EM_SPU, and the entry and every PT_LOAD address must be at least 0x3000, else NOEXEC. Contexts need 128-byte
  alignment for process SDK 0x280000 and up. A wake-up STAT becomes TASK STAT.
- **LS pattern helpers.** The D-041 semantics are confirmed: one bit per 2 KB block, six reserved, and a save area of
  1 KB + 2 KB per block. Corrections:
  - Only PT_LOAD segments with paddr below 0x40000 count.
  - Any program header past 0x40000 is INVAL.
  - The read-only pattern comes from `p_filesz`, not `p_memsz`.
  - `GenerateLsPattern` has no lower bound at 0x3000.
  - `GetContextSaveAreaSize` checks no pointers.

  GTA V's patterns are unchanged.
- **Taskset shutdown.** `cellSpursShutdownTaskset` refuses only workload IDs above 31 and passes the workload
  shutdown's result through (STAT becomes TASK STAT). `cellSpursShutdownWorkload` gains SRCH for unregistered
  workloads.
- **cellSheap** (supersedes D-039) is libsre's buddy allocator in guest memory: a 128-byte header, a bitmap with 2 bits
  per tree node, and 128-byte blocks. Allocate returns 0 on failure. Free merges buddies. A double free is a libsre
  assertion.
- **libsre's assertions.** Where libsre asserts (it prints the condition and aborts the process), pspocket stops with a
  fatal log naming it. Where libsre would read a bad pointer, pspocket refuses the call with a logged error, as in
  D-042.

## D-048 · 2026-10-04 · cellSysutil reports RPCS3's default console; the system cache lives next to hdd0

Runs 015 to 017. GTA V calls these functions right before and after GCM init. Implemented now: the 16 cellSysutil
functions it imports (callbacks, system parameters, system cache, video and audio output state) and the two
cellSysutilAvconfExt ones. Not implemented yet: dialogs, the on-screen keyboard, save data and BGM.

- **The console reported** is RPCS3's default configuration, which suits a fixed iPhone target:
  - display: one HDMI display, 1280x720 progressive at 59.94 Hz, 16:9, RGB;
  - sound: stereo LPCM at 48 kHz;
  - settings: English, enter on cross, parental control off, no PSN account (D-024);
  - two values differ from RPCS3: the nickname "PSPocket", and the user name, which comes from the host's
    `/dev_hdd0/home/00000001/localusername` when that file exists, else "User".
- **System events** go through the callbacks the game registers. `sysutil.hpp` queues events for other HLE code, and
  `cellSysutilCheckCallback` calls the callbacks on the game's thread through `call_guest` (D-045). Nothing sends an
  event yet: there is no system menu.
- **The system cache** follows RPCS3: `/dev_hdd1` is mounted on `caches/<TITLE_ID>_<cacheId>` under the hdd1 root.
  That root is the host's `/dev_hdd1` mount if there is one, else `hdd1` next to the host directory of `/dev_hdd0`,
  which is `inventory/<game>/hdd1` with `scripts/boot.sh`. RPCS3's relay and clear rules apply, with its in-use
  marker, which pspocket writes in every case, not only for a new cache.

## D-049 · 2026-10-04 · Input: one virtual controller the host feeds; no keyboard

Runs 018 and 019. `sys_io` (the 14 cellPad and cellKb functions GTA V imports, following RPCS3) reports one
controller, always connected, on port 0. It looks like RPCS3's DualShock 3: standard type, press-sensitive buttons,
motion sensors, high-precision sticks, rumble. The host sets its state with `hle::set_pad_input`
(`pspocket/hle/input.hpp`). The iOS app will drive it from a touch overlay or an MFi/DualSense controller. On the Mac
it stays at rest. A connected controller is the right default for an app that always shows a virtual one; games stop at
"connect a controller" otherwise.
- `cellPadGetData` reports changes the way RPCS3 does: the first read, any change, and every 10 ms with sensors on.
  Rumble is accepted and ignored for now.
- RPCS3's sys_config event thread is not reproduced: the controller counts as connected, assignment change included,
  from `cellPadInit`.
- No keyboard is connected, as with RPCS3's default null keyboard handler.

This settles how input reaches the game, not the open question of whether a hardware controller is required.

## D-050 · 2026-10-04 · Native SPURS runs SPU tasks: host workers and a taskset policy that follows libsre

Runs 020 and 021 (D-037 stage 3b). Each SPURS instance gets one host worker per SPU (`spurs_scheduler.cpp`). A worker
is a pthread with an SPU context and 256 KB of local store.
- **Selection.** A worker picks a workload by the SPURS kernel's rules (RPCS3 `spursKernel1SelectWorkload`), using the
  guest CellSpurs line 0 (ready counts, signals, the workload flag, contention). It updates that line through the
  reservation system.
- **Idle.** An idle worker sleeps in `ps_res_wait_change` on that line: any ready count, signal or flag change from PPU
  HLE, SPU code (PUTLLC) or another worker wakes it, which is what the SPURS kernel's idle loop waits on too.
- **Not reproduced.** The system service never runs: its message is acknowledged, and workload state is read directly.
  Pending contention is not tracked, and SPURS2 has no scheduler yet.

**The taskset policy** (`spurs_taskset.cpp`) follows libsre's taskset policy module, read from the firmware, field by
field. RPCS3's `cellSpursSpu.cpp` is an unwired draft that differs in several places (sparse context blocks, LS cleared
at every start, exit-handler sizes, the receive-flag syscall).
- What task code sees is libsre's: the kernel context at LS 0x100 and the taskset context at 0x2700, the task image
  with libsre's ELF-loader checks, and the registers at entry.
- The syscall entry at 0xA70 is a `stop` the worker takes: 0 exit, 1 yield, 2 wait for a signal, 3 poll, 4 receive the
  workload flag.
- The task bits change on the guest taskset line with libsre's state machine, and ready counts are updated after every
  request.
- A task that yields or waits is saved to its own context save area exactly as libsre saves it: 0x380 bytes, then the
  pattern's blocks, packed. It resumes on any worker with $3 = 0.

**Waking tasks from the PPU.** `cellSpursEventFlagSet` writes each released task's events and sends it a signal
(`_cellSpursSendSignal`), replacing D-040's deliberate fatal stop.

**Events to the PPU.** SPU code's interrupt-mailbox events (`sys_spu_thread_throw_event` / `send_event`) reach lv2
event queues through the instance's SPU port map (D-040).

**SPU runtime hooks.** Contexts can have their own stop and interrupt-mailbox handlers (`SPUContext::hooks`), so SPURS
workers don't take over the process-wide ones, which lv2's SPU threads will need.

**Tests and a bug found.** The tests run hand-assembled SPU tasks that exit, wait and resume, get woken by an event
flag, throw events, and yield to each other. They found a lost-update bug in the ready-count update: the running total
was changed inside a retried reservation update, so a retry after another writer's store dropped the increment. A
regression test forces those retries.

## D-051 · 2026-10-04 · Custom workloads: a game's own policy module runs on a worker

Run 021. GTA V adds a workload with its own SPU policy module (`cellSpursAddWorkload`: 0x11B0 bytes, priority 12 on
SPUs 0-3, contention 5). Since it is game code, the worker runs it rather than imitating it.
- **Loading and entry.** The image is loaded at LS 0xA00, and reloaded only when another module was there, as libsre's
  kernel does. It is entered with the kernel's registers: $0 = 0x804, $1 = 0x3FFB0, $3 = 0x100, $4 = its argument,
  $5 = the poll status.
- **Kernel entry points.** The kernel's entry points it can reach are `stop`s the worker answers: select (0x290, via
  `[0x1E4]`), its return (0x804) and exit (0x808, via `[0x1E0]`).
- **Work arrives** through `cellSpursReadyCountStore` and the workload flag (`_cellSpursWorkloadFlagReceiver`,
  `cellSpursGetWorkloadFlag`), both checked against libsre.
- **Offset fix.** The workload flag word is at CellSpurs 0x6C, inside the 16-byte CellSpursWorkloadFlag at 0x60; it
  was read at 0x60 before.

## D-052 · 2026-10-04 · cellAudio: RPCS3's ports and periods on a host thread; output through a host sink

Run 021. The 11 cellAudio functions GTA V imports follow upstream RPCS3:
- 8 ports (4 for SDK ≤ 3.5) from RPCS3's free-list stacks, with its checks and error codes;
- notify queues on keys from 0x80004D494F323221, with one CELL_AUDIO_EVENT_MIX event per period.

How pspocket does it:
- **Memory.** The read indices (big-endian u64 in a 64 KB area, 16 bytes per server index) and the port buffers are
  main-area allocations. RPCS3 maps lv2 shared memory instead.
- **Thread.** The audio thread is a host thread that runs no guest code, from `cellAudioInit` to `cellAudioQuit`; it
  also stops when the process exits. Every 256/48000 s it clears the block just played, advances the read index and
  notifies the queues. When it falls more than 100 ms behind, it skips the missed periods rather than bursting.
- **Output.** There is no output backend yet. `hle::set_audio_sink` (`pspocket/hle/audio.hpp`) gets RPCS3's stereo mix
  each period, and the iOS app will attach CoreAudio there.
- **Gap.** lv2 can't look a queue up by its IPC key yet, so only queues made by `cellAudioCreateNotifyEventQueue` can
  be registered. GTA V uses that one.

## D-053 · 2026-10-04 · Every run that doesn't crash ends with a report of what each thread waits on

Run 020 was the first GTA V run to end at its timeout instead of a crash, and its log could not say why the game
stopped making progress. Since then, `pspocket-boot` prints a thread and wait report:
- **When.** At the timeout, after a guest fault (with a 5 s watchdog, in case the faulting thread holds a lock), and
  every N seconds with `--report-every N`. `scripts/boot.sh` prints the report block from the log.
- **What, per thread.** Its state; what it is parked on, with the object's useful state (queued events, wanted bits
  against the pattern, owners, counts, the guest lwmutex owner word); and a guest backtrace from the PPC64 back chain,
  where every frame is checked before it is read.
- **What, for the RSX.** Its FIFO position and what it is blocked on (`Rsx::status()`).
- **How.** lv2 waiters record what they wait on, and each PPU thread records its current syscall. This costs a few
  atomic stores on the slow (blocking) path only (`runtime/lv2/report.hpp`).

## D-054 · 2026-10-04 · CellSpursJobChain follows libsre's layout, not RPCS3's

Job chains are created as libsre creates them (fw 4.92, read as the reference, D-046). RPCS3's `cellSpurs.h` and
`_spurs::create_job_chain` differ from libsre in four places, and a job chain policy that follows libsre depends on all
of them:
- **0x70 / 0x72.** 0x70 is sizeJobDescriptor (u16) and 0x72 maxGrabbedJob (u16). RPCS3 writes maxGrabbedJob at 0x70
  and nothing at 0x72, so a libsre-style policy would grab no commands and never advance.
- **0x24 autoReadyCount.** Create writes the attribute's autoSpuCount there; it selects the ready count policy.
- **haltOnError.** The attribute keeps the name at 0x34 and haltOnError at 0x38 (RPCS3 swaps them), and create ORs
  haltOnError into the chain's flags byte (0x2C, bit 2).
- **The job error codes** the policy raises (0x80410A0B..0x80410A1F) are RPCS3's names with libsre's meaning.

## D-055 · 2026-10-04 · Native job chain policy: libsre's contract, jobs run one at a time

Runs 022 to 028 (D-037 stage 5). `spurs_jobchain.cpp` runs a job chain on the native workers the way libsre's job
chain policy module (the one cellSpursCreateJobChainWithAttribute registers) does, without running it. RPCS3 has no
job chain policy (its `spursJobChainEntry` is empty).
- **What a job sees** is libsre's contract. Its binary sits at a 1 KB page from LS 0x4C00 and is entered at its first
  byte with $3 = its context (CellSpursJobContext2 at 0x4940: input, cache, output and scratch buffers, buffer counts,
  DMA tag, descriptor EA), $4 = the LS copy of its descriptor, $126 = the binary's LS address (job code is position
  independent) and $sp 0x30 below the top of its scratch + stack, with libsre's terminating frame. A job runs to
  completion and puts its own output. LS 0x80..0x17F stay free for job code and the kernel context stays valid.
- **The guest protocol** is libsre's, so several workers and the PPU agree on the chain. A visit takes the chain's lock
  byte (0x29) through the reservation system, edits a copy of line 0, executes the commands at pc as guest memory has
  them then (the PPU rewrites END and JTS in place) and releases the lock, merging back only the urgent command slots,
  isHalted and maxGrabbedJob. Every command libsre knows is implemented: JOB, NOP, RESET_PC, SYNC, LWSYNC and their
  label forms, SET_LABEL, NEXT, CALL/RET (three deep), FLUSH, JOBLIST, GUARD, ABORT, END, JTS and urgent commands.
- **Ready count.** After every visit: busy SPUs + 1 with autoReadyCount, the requested count without it, 0 at END or a
  blocked guard, 1 at a blocked SYNC.
- **Memory.** Buffers are libsre's 1 KB pages, best fit, from 0x4C00 up to the four descriptor buffers. Binaries and
  cache elements stay in LS between jobs while the policy stays on the SPU (libsre's LS cache, eight entries, LRU), so
  a binary's data keeps its values from one job to the next. The previous job's scratch + stack is kept only for a job
  of the same size, and freed before the next job's buffers otherwise.
- **Errors** follow libsre's error path: isHalted, the workload shut down (unless the chain halts on errors), the error
  and cause at jc+0x80/0x88. With haltOnError (GTA V's chains) libsre halts the SPU, so it is fatal here.

Deviations, all because jobs run one at a time on a worker:
- A job's descriptor, binary and input are loaded when it starts, not while the previous job runs.
- An SPU that leaves the chain after running jobs makes one more visit without taking jobs, so its busy bit does not
  hold up a SYNC on another SPU.
- When a job's buffers don't fit as the pages are laid out (GTA V's Secondary chain has jobs that need 232 of the 234
  pages), the job is laid out again in an emptied heap (cached binaries and the kept scratch + stack dropped). libsre
  makes room by rolling back what it fetched ahead.
- A visit that changes nothing (a JTS waiting for the PPU) is followed by a short sleep instead of libsre's spin.

Not implemented (fatal when a chain asks for them): the memory check (jobType bit 1, jobMemoryCheck), trace packets,
the exception event handler.

## D-056 · 2026-10-04 · Polls consume the current workload's signal; SPU code sets event flag bits

Two scheduler changes found by GTA V's job chains (runs 022 and 023):
- **Polls.** When a worker's poll keeps the workload it runs, the poll consumes that workload's signal and the workload
  flag, as SPURS kernel 1 does (libsre select: not a poll, or the selection is the current workload; RPCS3
  `spursKernel1SelectWorkload`). Before, only a dispatch consumed them, so a chain kicked again while it ran kept its
  SPU after its END forever.
- **Event flag bits.** SPU code's interrupt-mailbox codes 128 (`sys_event_flag_set_bit`, answers in the inbound
  mailbox) and 192 (`..._impatient`, no answer) set bit `value & 0xFFFFFF` of the lv2 event flag whose ID is in the
  outbound mailbox (RPCS3 SPUThread.cpp `set_ch_value`). GTA V's Secondary chain wakes its audio mix thread this way.

## D-057 · 2026-10-04 · The Mac shows the game's frames as PNG files through the Metal backend

Run 029. `pspocket-boot --gpu metal` gives the RSX the Metal backend (D-032) instead of the null backend, on Apple
platforms; `--frames DIR [--frame-every N]` (which implies Metal) saves every Nth presented frame as
`DIR/frame-NNNNNN.png` (60 by default). That is the first picture: GTA V's Rockstar logo screen.
- **Capture.** The Metal backend's flip copies the output texture (1152x640, BGRA8) to a shared buffer in the frame's
  own command buffer and hands it to a callback once the GPU has finished it (`MetalBackendConfig::capture`).
- **PNG.** `pspocket/core/png.hpp` writes 8-bit RGB PNGs with stored (uncompressed) deflate blocks: no library, any
  viewer reads them. Frames stay in `inventory/` like every other game output.
- **Why files.** pspocket-boot is a command-line tool: a window needs an app run loop. The files are enough to see
  what the game draws while the boot loop goes on; the iOS app presents through the same backend.
- **Tests.** The Metal backend's end-to-end tests (`tests/rsx/test_metal.mm`) were in the tree but in no build; they
  now run in the host build on Apple platforms (`test_rsx_metal`), with a capture test.

## D-058 · 2026-10-04 · cellSpursEventFlagSet takes PPU2SPU flags; CellSpursTaskset2 through the attribute's size

Two cellSpurs PPU-side fixes from runs 029 and 030, both checked against libsre (fw 4.92, D-046):
- **Event flag set direction.** libsre's cellSpursEventFlagSet accepts PPU2SPU and ANY2ANY flags. RPCS3 (and pspocket
  until now) check SPU2PPU and ANY2ANY, the directions a PPU *wait* needs, and refuse a PPU2SPU flag with PERM. GTA V's
  DVD streamer hands work to its zlib task through a PPU2SPU flag, ignored the PERM, and waited for the answer forever:
  nothing loaded and the screen stayed black. With libsre's check the game loads and draws its logo screen.
- **Taskset2.** `cellSpursTasksetAttributeSetTasksetSize` takes 6400 (CellSpursTaskset) or 10496 (CellSpursTaskset2),
  as libsre and RPCS3. A taskset created with the larger size gets, as in libsre, the descriptor of the exit code
  container handler at 0x1900: a marker (0x400) here, since that handler is Sony SPU code and no task of GTA V has an
  exit code container (the taskset policy stops loudly if one does).

## D-059 · 2026-10-04 · PSN offline: 43 of GTA V's 51 sceNp functions, as RPCS3 without a signed-in user

GTA V calls PSN functions one after another during startup (runs 026 to 028). Instead of one round per function, the
sceNp functions it imports with a simple offline behaviour in RPCS3 are implemented together (D-024: PSN is a
non-goal). Each makes RPCS3's checks in RPCS3's order, then takes RPCS3's path with PSN disabled: status offline, NP ID
all zeros, no ticket, no basic event, no signaling connection, addresses 0.
- **Lookup:** init and term, title contexts (IDs from 0x3001 as RPCS3's idm hands them out), transaction contexts
  refused (INVALID_ONLINE_ID), the async requests refused, poll reports done.
- **Manager:** NP ID, region, age, content rating, chat restriction and network time: SCE_NP_ERROR_OFFLINE; no ticket;
  no entitlements; the callback kept, never called.
- **Basic:** one event handler kept (never called); no events; presence accepted and dropped; no block list or
  messages.
- **Signaling:** contexts (IDs from 0x6001, at most 8), handlers kept, local net info with no addresses, no
  connections. **Util:** sceNpUtilCmpNpId.

Left out, until a log shows them: functions that open system dialogs, send messages, need RPCN or the network
(sceNpSignalingActivateConnection), write player history with a later event, or check DRM files and keys
(sceNpDrmIsAvailable).

Where pspocket differs from RPCS3: an invalid communication ID in sceNpLookupCreateTitleCtx is logged and refused
(RPCS3 aborts); a 33rd title context gets TOO_MANY_OBJECTS (RPCS3 returns 0, which reads as success); an unreadable
presence pointer is refused (RPCS3 faults); presence data is not kept (nothing reads it offline).

## D-060 · 2026-10-04 · cellRtc as RPCS3, quirks included; the clock is lv2's

The seven cellRtc functions GTA V imports (current tick and local clock, tick and date conversions, Win32 file times,
RFC 2822 formatting, the date parser) follow RPCS3's cellRtc.cpp, which follows libcellRtc closely, including its
quirks: a "+hhmm" time zone reads as hh * 60 + hhmm % 60 minutes, a day below '0' in asctime dates passes, hours up to
25 are accepted. The current time and time zone are what lv2's sys_time_get_current_time and sys_time_get_timezone
report: both now read the host clock through `lv2::host_current_time` and `host_time_zone`. Pointers are checked
against mapped guest memory, as RPCS3 does with sys_memory_get_page_attribute.

## D-061 · 2026-10-04 · Voice, trophies, network control, background music: offline as RPCS3

The small gaps of runs 029 and 030. Each function makes RPCS3's checks in RPCS3's order and takes its path with no
network, no PSN and no voice devices.
- **cellVoice** (14 functions): the game's ports kept as data with RPCS3's IDs (a creation counter mod 0xA0 in the
  low byte) and its input/output topology checks; the device reads as detached, reads return nothing, writes go
  nowhere.
- **cellNetCtl** stays disconnected: GetInfo answers only the Ethernet address (FF:FF:FF:FF:FF:FF, as RPCS3 before it
  finds one), NAT info is RPCS3's fixed type 2 with STUN OK, the network start dialog reports LOADED and FINISHED
  500 ms after the load and is canceled at unload. The two events go out from the first `cellSysutilCheckCallback`
  500 ms after the load, rather than from a helper thread as in RPCS3, so no host thread outlives the process.
- **sceNpTrophy:** init, term, contexts and handles with RPCS3's IDs and communication-signature checks. Installing
  TROPHY.TRP from the disc (RegisterContext) and the calls that read or write the installed set wait for a log to show
  them.
- **Background music:** RPCS3's state, starting disabled with the player stopped. `sys_net_free_thread_context`
  returns CELL_OK.
- Where RPCS3 would fault on an unchecked guest pointer, the call site is logged and the call refused with the
  module's address error (D-042).

## D-062 · 2026-10-04 · Save data on host directories in RPCS3's layout; no dialogs yet

GTA V calls cellSaveDataAutoLoad2 during startup. The six save data functions it imports (cellSysutil's
cellSaveDataAutoLoad2, AutoSave2, ListAutoLoad, Delete2 and FixedDelete, and cellSaveData's cellSaveDataGetListItem)
follow RPCS3's cellSaveData.cpp in `runtime/hle/src/cellSaveData.cpp`, through one `savedata_op` that makes RPCS3's
argument checks with the PS3's error numbers.
- **Layout:** RPCS3's, so saves move between RPCS3 and pspocket unchanged: `/dev_hdd0/home/00000001/savedata/<dir>/`
  (`inventory/<game>/hdd0` with boot.sh) with PARAM.SFO (category SD, title, subtitle, detail, list parameter, and
  RPCS3's `*<file>` secure-file flags and RPCS3_BLIST file order) and the game's files.
- **Callbacks** run on the calling thread through call_guest (D-045), their structures in one guest block in the main
  area: funcFixed picks a listed save, funcStat sees it and may set PARAM.SFO, funcFile reads, writes or deletes one
  file per call until OK_LAST. A changed save is written whole to `.working_<dir>` and swapped in through
  `.backup_<dir>`, so an interrupted save leaves the old or the new one.
- **No dialogs yet:** the auto and list-auto variants need none; cellSaveDataDelete2 returns CELL_CANCEL and
  cellSaveDataFixedDelete without OPTION_NOCONFIRM returns CELL_SAVEDATA_ERROR_INTERNAL, RPCS3's results with no
  dialog. Errors the PS3 would show in a dialog are logged.
- **Differences from RPCS3:** its PS3-imitating sleeps are left out (as D-044) except the 100/500 ms after the last
  funcFile that games rely on; the savedata directory is made by the first call, not at boot; host failures, a fixed
  delete of an unlisted save, a null funcDone and pointers outside memory return errors where RPCS3 throws or faults;
  saves are listed in name order.

Left out until a game imports them: the list and fixed variants with dialogs (funcList), the User* variants, import
and export, and the auto-save indicator.

## D-063 · 2026-10-04 · SPURS queues as libsre: one shared line, marked indices, the PPU woken through a SPURS port

Run 030 stopped at `_cellSpursQueueInitialize` (the game asserts when it fails). RPCS3 has no SPURS queues (stubs, and
a six-argument initialize), so the four functions GTA V imports follow libsre (fw 4.92, read as the reference, D-046)
and the SPU-side queue library its tasks link statically, in `runtime/hle/src/cellSpursQueue.cpp`.
- **Layout.** CellSpursQueue is one 128-byte line: pop and push indices counting modulo 2 × depth (so full and empty
  differ), entry size, depth, the entries' address, the SPU port of the attached lv2 queue, the direction, FIFO lists
  of waiting SPU tasks (task IDs and workload IDs, at most 15), the taskset, CellSpurs and the lv2 queue ID.
- **Protocol.** A push or pop is two atomic updates of the line with the copy between them. The first stores `~index`,
  a claim that gives another PPU caller BUSY and makes an SPU task wait; the second stores index + 1. Waiting SPU tasks
  join the list and wait for a signal (taskset syscall 2, wait object `queue | 4`); whoever frees an entry signals the
  oldest. The PPU keeps its index marked for its whole blocking call and waits on a one-event lv2 queue ("_spuQue",
  FIFO) on a dynamic SPU port; an SPU push into an empty queue whose pop index is marked throws
  `sys_spu_thread_throw_event(port, 0, 0)`.
- **Native.** The SPU side is task code the native taskset policy already runs (GETLLAR/PUTLLC, DMA, syscall 2, its
  own signalling, the interrupt-mailbox event); its event reaches the PPU through the instance's port map (D-040,
  D-050). A synthetic SPU task test checks that each side wakes the other.
- **Shared helpers.** `attach_lv2_eq` and `detach_lv2_eq` are now `spurs::` functions (libsre also uses one helper for
  event flags and queues).
- **Deviations.** Unreadable pointers give TASK_ERROR_FAULT before an entry is claimed (D-042); libsre's asserts stop
  with a fatal log (D-047); a failed lookup of a waiter's workload returns FATAL where libsre signals an undefined
  taskset.

Left out until a game imports them: PushBody (the PPU's side of PPU2SPU queues), Clear, Size, Depth, the getters and
the LF queues; their semantics are summarised in the file header.

## D-064 · 2026-10-04 · The SPURS kernel weighs 16 - priority, as libsre's system service gives it

Run 031: GTA V's audio engine thread waited 64 s for a job of its "AudioEngine" job chain (priority 4 on SPUs 0-3) that
never ran, and the audio mixer then crashed on a voice the engine had not set up. The native scheduler (D-050) weighed
the workloads' priority bytes as CellSpurs holds them, where 1 is the highest and 15 the lowest, so GTA V's own policy
module (priority 12) and the Bink tasksets won every SPU over the job chains. libsre's system service (fw 4.92, read
as the reference) writes 16 - priority into each SPU's kernel context, 0 staying "not on this SPU", and the kernel's
selection weighs those (RPCS3 spursSysServiceUpdateWorkload). The worker now converts as it reads the guest state, and
the kernel context at LS 0x1A0 holds the converted bytes, as policy and task code see them on the PS3. Workload
creation logs show the priorities. A regression test runs two tasksets on one SPU: priority 2 goes before 12.

## D-065 · 2026-10-04 · Trophies as RPCS3: the disc's TROPHY.TRP installed into the user's trophy directory

GTA V creates its trophy context ("NPWR03885_00"), asks for the disk space it needs and registers it. The five
functions it calls next (GetRequiredDiskSpace, RegisterContext, GetGameInfo, GetTrophyInfo, UnlockTrophy) follow
RPCS3's sceNpTrophy.cpp, with RPCS3's context and handle checks in its order.
- **The pack.** `TROPDIR/<set>/TROPHY.TRP` is read through lv2's file system, so it is read in place from the disc
  image (an HDD game's own TROPDIR, or the disc's when the PARAM.SFO category is GD), and checked as RPCS3's TRPLoader
  does, SHA-1 included (`trophy_files.cpp`; `pspocket_hle` now links recomp_common for the hash).
- **Installation** into `/dev_hdd0/home/00000001/trophy/<set>`: `TROP_01.SFM` (English, cellSysutil's language), else
  `TROP.SFM`, becomes `TROPCONF.SFM`; the other language files are left out; the icons are copied. The files go to a
  temporary directory that then replaces the set's. `TROPUSR.DAT` keeps the user's progress across installations, in
  RPCS3's layout, with its platinum rule. No TROPTRNS.DAT, as RPCS3.
- **Registration** calls the game's status callback once directly, then sends RPCS3's SETUP, PROGRESS, FINALIZE and
  COMPLETE steps through `cellSysutilCheckCallback` one at a time; the caller blocks for at least 2 s, plus up to 50 ms
  for each step no check has taken (RPCS3's timing: about 7 s when the registering thread is the one that checks).
- **Details and unlocks** work on the installed set: game info, trophy info (hidden trophies refused until unlocked),
  unlocks with RTC ticks and platinum completion.
- **Deviations.** Pointers are checked before use (D-042); a TROPUSR.DAT that can't be created gives ILLEGAL_UPDATE
  where RPCS3 aborts; without an error dialog, SHOW_ERROR_EXIT asks the game to exit at once (what RPCS3 does when its
  dialog can't open). No trophy pop-ups, trophy list or RPCN sync. The XML reader is a small one that matches what
  RPCS3 gets from pugixml.

## D-066 · 2026-10-04 · PPU lwarx/stwcx. go through the shared reservation system

Run 033: two GTA V audio threads took the same job slot from a lock-free stack (a byte array of next indices, the
head word updated with lwarx/stwcx.) and ran two jobs in one descriptor. The PPU's reservations were still the
single-thread placeholder D-023 left open: stwcx. compared the reserved word and then stored it as two separate steps,
by value only, so two threads could both succeed, and a pop could succeed over a pop-pop-push of the same head (ABA).
- **Now** lwarx/ldarx take a snapshot of the 128-byte line and its version from the shared reservation system (D-030)
  into the PPU context (`reserve_line`, `reserve_version`; the context grows to 0x4F0 bytes), and stwcx./stdcx. store
  through `ps_res_store_conditional`: atomically, only while no conditional store, SPU PUTLLC or notified write changed
  the line and its bytes still equal the snapshot. PPU threads, SPU atomics and HLE updates of guest sync objects
  (`reservation_op`) now see one another. As RPCS3: the address must be the reserved one and aligned; the reservation
  is consumed either way.
- **Tests.** A regression test runs four interpreter threads through a lock-free stack and a shared counter; the old
  code lost more than half of 80,000 increments and handed one slot to two threads. The differential fuzz harness now
  takes real reservations on each side (the store on one side moves the line's version on for the other).

## D-067 · 2026-10-04 · /dev_flash is the user's own firmware; SPU faults name their thread, code and memory

Runs 034-038 ended in an SPU DMA from address 0 in GTA V's audio mixer job. The job was loading its MP3 decoder from a
96 KB buffer that was all zeros: the game's statically linked MultiStream library fills it from
`/dev_flash/sys/external/flashMP3.pic` (Sony's MP3 codec, SPU code), and pspocket had no /dev_flash, so the load failed
quietly ("cellMSMP3Decoder: Error loading MP3 codec") and the mixer later ran the empty buffer.
- **/dev_flash.** As in RPCS3, system files a game reads itself come from the user's own firmware: plan section 8
  allows LLE of firmware code with the user's file, kept out of the repository. The local firmware script
  (`inventory/firmware/tools/fw_unself.py --raw flashMP3.pic`) copies such files as they are into
  `inventory/firmware/dev_flash`; `pspocket-boot --flash DIR` mounts it read-only and boot.sh passes it when it exists.
  Only what a run shows the game opening is taken (CLAUDE.md rule 4): so far flashMP3.pic. SPURS stays native
  (D-037); a native MP3 module could replace this one later. The iOS app will need the same file from the user's
  firmware, as it needs the user's disc.
- **Fault reports** from code that isn't a PPU thread now name the host thread (`SPURS 10016000 SPU 0`) and, for SPU
  code, its pc and instruction, the MFC parameters and the registers' preferred words (`ps_spu_current`).
  `--dump-on-fault FILE` writes the guest memory below the RSX's (and the faulting SPU's local store) for offline
  reading; it is game data and stays in inventory/. Job chain fatal logs dump the job's descriptor.
- Reloads of a game's own SPURS policy module are logged once per SPU, then at debug level (thousands per run now
  that priorities are right, D-064).

## D-068 · 2026-10-04 · Fragment program condition codes, as RPCS3

Runs 031 and 039 showed a grey screen: every draw of GTA V's frontend shaders failed on `MOVR RC0, ...`, a condition
code update (110,146 skipped draws in run 039). The MSL translator now follows RPCS3's FragmentProgramDecompiler
(SetDst, AddCodeCond, AddFlowOp, GetRawCond): condition registers RC0 and RC1 start at 0; a conditional instruction
compares RC{cond_reg}, through its condition swizzle, with 0 (>=, <=, !=, >, < or == from its three condition bits)
and writes only the destination components whose test holds (`select`); a conditional KIL discards when any component
holds; set_cond copies what an instruction wrote to RC{cond_mod_reg}, or without a destination its result (scale only,
no clamp) when any component's test holds. Run 040: no failed draws; the legal screen and the Display Calibration
screen draw as on the PS3.

## D-069 · 2026-10-04 · Scripted controller presses for runs on the Mac

Run 040 stops at GTA V's Display Calibration screen, which waits for Cross. `pspocket-boot --press
BUTTON@START[+EVERY][:HOLD]` (repeatable) drives the virtual controller (D-049) on a schedule: `cross@45+10` holds
Cross for 0.25 s at 45 s and every 10 s after; `start@30:1.5` holds Start for 1.5 s. The press-sensitive buttons also
report full pressure. The schedule (`pspocket/hle/pad_script.hpp`) is checked every 20 ms and each change is logged
("controller at 45.00 s: cross"); boot.sh passes the option through. Times are wall-clock seconds since the game
started, so a schedule only holds for one build's speed: it is a bring-up tool, not a replay system.

## D-070 · 2026-10-04 · SPURS semaphores as libsre: a unit given back goes to the oldest waiter

Run 041 stopped loading at `_cellSpursSemaphoreInitialize`: GTA V gives its "Physics" taskset a semaphore of 4 units,
the call failed, and the Physics tasks waited in P on a semaphore with no units while the main thread waited for them
in `cellSpursQueuePopBody`. RPCS3 has only commented stubs, so `runtime/hle/src/cellSpursSemaphore.cpp` follows libsre
(fw 4.92, read as the reference, D-046: `_cellSpursSemaphoreInitialize` 0x17F04) and the SPU-side semaphore library
the Physics tasks link statically.
- **Initialize** (spurs, taskset, semaphore, total): cellSpursSemaphoreInitialize passes spurs 0, the IWL form taskset
  0. With spurs: semaphore NULL, then spurs and semaphore alignment (128); the taskset is only stored. Without:
  taskset or semaphore NULL, their alignment, then a workload ID above 31 (INVAL). The total is stored unchecked.
- **Layout.** One 128-byte line: the count at 0x00; the waiting tasks' IDs from 0x10 (their number first, oldest first,
  15 at most) and their workload IDs from 0x20; CellSpurs at 0x30 for a semaphore of the whole instance (else 0); the
  taskset at 0x38 (0 for the whole instance); the rest zero.
- **Protocol.** Only SPU tasks take and give units (libsre has no PPU P or V). P takes one when the count is above 0;
  otherwise it joins the lists (AGAIN at 15) and waits for a signal (syscall 2, wait object `semaphore | 1`). V, when
  tasks wait and the count isn't negative, hands its unit to the oldest: the waiter leaves the lists, the count stays,
  and V signals it from SPU code (its signalled bit and, if it waits already, its workload's wklSignal bit); otherwise
  V adds 1 to the count. The woken task doesn't look at the count again. Its taskset is the semaphore's, or for the
  whole instance its workload's (WorkloadInfo.arg).
- **Native.** All of that is task code the native taskset policy already runs; nothing was missing. Synthetic SPU tasks
  check it on four SPUs: more tasks than units with both kinds of semaphore, a signal that arrives before the waiter's
  syscall 2, and a waiter whose taskset has nothing else to run, brought back only by the workload signal a task of
  another taskset sets.
- **Deviations.** Unreadable pointers give TASK_ERROR_FAULT (D-042). The line's reservations are broken explicitly, as
  the PPU's stores break them on the PS3.

Left out until a game imports it: cellSpursSemaphoreGetTasksetAddress.

## D-071 · 2026-10-04 · The iOS app boots a game: the disc image picked in Files, the executable from the Mac

The user asked to try GTA V on the iPhone. The app's new Game tab (now the first tab) does what pspocket-boot does on
the Mac, through `pspocket/platform/game.h` (`runtime/platform/ios/src/game.mm`, Objective-C++).
- **Disc image.** Picked in Files (`fileImporter`), read in place (no copy of 20 GB), kept open through its
  security-scoped URL and remembered with a bookmark; probed for its title ID and title.
- **Executable.** The disc's EBOOT.BIN is an encrypted SELF, and the keys to decrypt it never go into the repository
  or the app. `scripts/ios.sh push-game` copies the ELF decrypted on the Mac to `Documents/game/<TITLE_ID>/EBOOT.elf`,
  with the /dev_flash files the game loads itself (D-067; not the reference-only .sprx). Both stay on the user's
  devices.
- **Runtime.** The app's runtime library now compiles the same components as pspocket-boot: lv2, the HLE modules,
  the SPU runtime, the RSX front end and Metal backend, the loader, and recomp/common with the host build's embedded
  NID tables. PPU and SPU code run on the interpreters (no lifted code yet).
- **Frames.** The Metal backend's output texture (1152x640, D-016) is copied into a CAMetalLayer from its present
  callback at each flip; Core Animation scales it to fit. Once the game runs, the app shows it full screen and locks
  to landscape (status bar and home indicator hidden). No MetalFX or frame pacing yet.
- **Input.** A game controller paired with the phone (DualShock 4, DualSense, Xbox, MFi) drives the virtual
  controller through GameController (D-049): A/B/X/Y as cross, circle, square, triangle; Menu (Options) is Start;
  Options (Share, Create) and the touchpad click are Select; both sticks; L2/R2 and the D-pad with pressure. Without
  one, on-screen buttons lie over the game, each down while touched; they hide while a controller is connected.
- **Log.** `Documents/game.log`, written from scratch at each boot; a double tap shows its last lines over the game.
  A guest fault adds one line to it, then gets the system's crash report.
- One game per process; quitting the app stops it.

Run 042 is the first device run: the same screens and the same stopping point as the Mac, at the Mac's pace. The
`--press` parser now uses strtod: iOS 17's libc++ has no floating-point `std::from_chars`.

## D-072 · 2026-10-04 · Sound out through AVAudioEngine, on the Mac and the iPhone

cellAudio already mixed every started port into one stereo block per period and handed it to a host sink (D-052);
nothing played it. `runtime/platform/apple` (macOS and iOS) now does: an `AudioRing` (`pspocket/core/audio_ring.hpp`,
lock-free, one producer and one consumer) takes the 256-frame periods from cellAudio's thread, and an AVAudioEngine
source node pulls them on the device's render thread, 48 kHz stereo float (the main mixer converts to the device's
format). Playback starts once 2048 frames (43 ms) are queued and waits for them again after running dry, so an underrun
plays silence instead of crackling; a period that doesn't fit is dropped (an overrun). The ring holds 16384 frames.
`pspocket-boot --audio` plays on the Mac and prints the played time, underruns and overruns: a 75 s GTA V run played
72.3 s with one underrun and no overruns. The iOS app sets the playback audio session category (sound with the silent
switch on) and attaches the same output when it boots a game.

## D-073 · 2026-10-04 · Idle SPURS workers sleep: a selection that takes nothing stores nothing

Found while writing the SPURS semaphores (D-070), and the first item of run 042's notes: an idle worker's own look at
CellSpurs line 0 stored the line even when it took nothing. Every store moves the line's version on, so the worker's
wait for a change (D-050) ended at once and idle workers spun at full speed: on the iPhone, six emulated SPUs took a
full core each. `Worker::select` now stores the line only when it changed something (a workload taken, a signal or
the workload flag consumed, contention moved, the system service message acknowledged). Four idle workers: 7.8 s of CPU
in 2 s before, none after; the SPURS tests under parallel load: 172 s of CPU before, 39 s after. Work arriving through
the reservation system (HLE updates, PPU stwcx., SPU PUTLLC and DMA, cellSpursWakeUp) still wakes a worker at once; a
plain store to line 0 is seen at the idle wait's 100 ms timeout. A regression test checks that the line stays
untouched while nothing happens.

## D-074 · 2026-10-04 · cellAudio follows the speaker and waits for the game: RPCS3's buffering mode, per-port waits

The sound on the iPhone buzzed ("like bad radio") on the loading screen. cellAudio played a period every 5.33 ms by the
clock (RPCS3 without buffering, which is what RPCS3's null renderer does) and never looked at whether the game had
written the block. GTA V adds each block with `cellAudioAddData`, which adds to the block at the read index, once per
`CELL_AUDIO_EVENT_MIX`. A game thread that answers an event a little late finds the read index already moved on: the
block it was meant for plays silent, its data lands on the next block, and if it then answers the next event at once,
on top of the same block. Silent and doubled blocks alternating at up to 94 Hz are the buzz.

With a sink that can say how much it holds, cellAudio now runs RPCS3's buffering mode with its default settings (a
34 ms buffer, no time stretching; the decisions in `runtime/hle/src/audio_buffering.hpp`, the loop in cellAudio.cpp):
- Each played block gets RPCS3's tags: -0.0f on six floats spread over the block (and, on 8-channel ports, marks on
  the front right channel for games that fill only the front channels). Before mixing, the thread looks at the next
  block's tags: written, being written (a tag moved since the last look), or untouched.
- The period's length keeps the host's buffer at its target: 34 ms, or the device's callback plus two blocks when that
  is more, half a period to 1.2 periods depending on how full the buffer is. A restart queues the target's worth of
  silence first.
- A block being written gets 0.5 ms more; an untouched one is waited for, 1 ms at a time. A period whose blocks stay
  untouched too long goes on without them (all untouched: skipped, nothing played).

Which untouched ports to wait for differs from RPCS3. RPCS3 counts the untouched ports it has given up on
(`untouched_expected`, held for 340 ms after each match) and waits only while more are untouched. The first Mac run
with it (run 043, `boot-042.log`) found the count stuck: GTA V opens a second port, starts it, never writes it and
closes it 20 s in, and from then on the count stayed at one, so the main port's late blocks played silent without
waiting, half the periods in a row: the buzz again. RPCS3's recompiled PPU answers in time, so its count rarely
matters; here the game's audio thread answers late often. Now each port says whether the game feeds it (a tag moved
in the last 340 ms; a port's tag state starts fresh at `cellAudioPortStart`), and the loop waits for every untouched
port that is fed, for RPCS3's 2 periods (4 when other ports are ready). A port the game never wrote, or stopped
writing, is never waited for. Three 100 s Mac runs with waits of 0, 2 and 8 periods gave the same game audio (about
9,500 `cellAudioAddData` blocks, 64% of real time in the loading stretches and 20% in the heaviest), and none of them
a doubled block (two `cellAudioAddData` in one played block); the wait only decides how often a period goes on without
the game. So the limits stay RPCS3's.

Without a query (no sink, a capture in the tests, `pspocket-boot` without `--audio`) the fixed clock stays, without
tags. The sink's query (`hle::AudioBufferQuery`) returns the frames queued and the device's callback length, which
`AudioRing` now records as the largest pull. On iOS the app asks for a 5.33 ms IO buffer (one period; iOS's default of
about 20 ms would swing the 34 ms buffer by more than half) and logs what it got. cellAudio logs a sound line every
5 s in buffering mode (periods mixed, how many of them not silent, silent and skipped; how many waited for the game and
the longest wait; the `cellAudioAddData` calls and the blocks that got two or more; the range the host's buffer stayed
in), and the iOS FPS line counts the speaker's underruns. What this can't fix: a game whose
audio runs slower than real time for long stretches lets the speaker run dry (gaps, not buzz); RPCS3's answer is time
stretching, not done yet.

## D-075 · 2026-10-04 · Time stretching while the game is behind: the ps3-ios-engine port's follow-the-game tempo

D-074 plays the game's blocks whole, but GTA V delivers its audio slower than real time on the loading screens (55 to
75% on the Mac, 20% in the heaviest stretch), so the speaker kept running dry: about 500 underruns in 100 s. RPCS3
answers that with time stretching (an option, off by default): SoundTouch slows the output while its buffer is low,
moving the tempo halfway to the buffer's fill in steps of 0.1, which swings between the game's pace and real time. The
ps3-ios-engine port replaced that rule with one that follows the game, tuned on the iPhone, and that is what runs here
(`audio_buffering.hpp`, `Stretch`):
- The game's pace is measured from the periods, averaged over 0.7 s: a period that waited for the game counts at
  nominal / actual length, one that didn't at least 1 (a period stretched to 1.2 because the buffer is full isn't
  slowness), a skipped one at 0; periods of half a second or more are pauses and don't count.
- Stretching starts below a pace of 0.95 (or with the buffer under 30% full while the game isn't ahead) and stops once
  the pace has stayed at 0.98 or more with the buffer at least 60% full for 2 s: every switch splices the audio once.
- While on, the tempo is the pace trimmed toward the target fill (30% per 100% of fill error), 1 from 0.98 up, never
  below 0.25 (slower than that, the speaker runs dry instead); changes under 0.02 aren't passed on.
- cellAudio counts what the host holds at the speaker's tempo (it lasts 1 / tempo longer), for the period length too.

The stretcher is the host's: a sink may come with an `hle::AudioStretch` (on or off, and the tempo).
`platform::AudioOutput` puts an AVAudioUnitTimePitch between its source node and the mixer, bypassed while off, so the
audio is untouched while the game keeps up. Run 045 (`boot-047.log`, 120 s on the Mac): the pace went from 1.07 at the
opening to 0.7 on the loading screens and 0.2 at the heaviest, the tempo followed it down to the floor, and the speaker
ran dry 69 times instead of about 500.

Also: SPURS task creation, task starts and semaphore setups are logged the first 8 times and every 1024th. In the first
mission GTA V starts thousands of tasks a second, and run 044's mission log grew by 80,000 lines in a few minutes.

## D-076 · 2026-10-04 · The game's SPU programs run lifted: collected on the Mac, lifted per image and LS address

The iPhone's intro ran at 8 to 12 fps after D-073, and a Time Profiler capture during loading put 76% of the CPU in
the SPU interpreter (`execute` 55%, its per-instruction support check 11.5%), with SPURS selection down to 3%: what is
left is the game's SPU work itself (Bink video, the audio mixer, physics, decompression). The SPU lifter (D-029) and
the runtime's program registry existed, but nothing collected GTA V's programs or attached them, and the SPU
differential fuzz test was never in the build. Now:
- The SPU fuzz test builds and runs (`test_spu_fuzz`, 256 cases per instruction, CLAUDE.md rule 3); it passes.
- SPURS attaches the lifted program for every image it loads into a worker's local store: a task's first executable
  segment (at start and at resume), a job's binary, a game's own policy module (`runtime/hle/src/spu_programs.cpp`).
  An image is known by the hash of its code in guest memory, not of the worker's copy (a job keeps its data inside its
  binary, so the copy changes as it runs), and by its LS address: lifted code is specific to it, and
  position-independent job binaries land on different pages, so one binary can make several programs.
- An image without a lifted program is logged once and interpreted, in every build. CLAUDE.md rule 5 asks dev builds
  to abort on an unknown SPU hash; that waits until the collection is complete. Code a program reaches outside its
  lifted range (the SPURS syscall stubs, code it loads itself) is interpreted too (`PS_SPU_MISS_INTERPRET`).
- A DMA GET over the attached program's code drops it for the rest of the run: an overlay or other code loaded there
  would leave the lifted code stale.
- `pspocket-boot --spu-dump DIR` saves each image without a lifted program (the code, and its kind, guest address, LS
  address, size and entry); `scripts/lift-spu.sh DIR OUT [LOG...]` lifts them all (seeds from earlier runs' "no
  lifted code at" lines) and writes `game_spu.c`, whose `pspocket_game_spu_register()` registers them;
  `-DPSPOCKET_GAME_SPU=OUT` builds them into pspocket-boot. Images and lifted code come from the user's game and stay
  in the gitignored `inventory/` (CLAUDE.md rule 1), like the decrypted executable.

## D-077 · 2026-10-04 · Memory ordering between the SPUs, the PPU and the RSX on ARM

Run 048 (the first mission on the Mac with the null backend) ended on "malformed FIFO command 0x80000000", run 049 on
"invalid primitive 80" and then a malformed command: the RSX read stale command words. The Cell orders these hand-offs
itself; the host must make them hold on ARM, where, unlike x86, plain stores can become visible to another core out
of order and plain loads can be satisfied out of order:
- The RSX reads put, the word at get (a command, or a jump the guest just patched to release the RSX) and the
  semaphore it waits on with acquire loads: what it reads next is at least as new.
- Every MFC PUT is preceded by a release fence. GTA V's graphics jobs write RSX commands from SPUs, then the jump or
  flag that hands them over; the MFC performs a queue's transfers in order, and with plain copies a later PUT could be
  seen before an earlier one. Barrier commands (`barrier`, `eieio`, `sync`) were already full fences, and PPU `sync`
  too.
A FIFO error stays fatal in dev builds (CLAUDE.md rule 5): if one comes back it is another bug. A stress test appends
commands over malformed placeholder words while the RSX thread consumes them; it can only fail when the ordering is
broken, and it didn't reproduce the race without the fix in 30 runs either, so the fix rests on the memory model.

## D-078 · 2026-10-04 · The first mission's shaders and textures, as RPCS3

Run 044 reached the prologue with the 3D world black: its shaders and render targets use features the Metal
translation refused, and a refused program skips the draw. Added, each following RPCS3 (upstream 431b169):
- Vertex program condition codes (VertexProgramDecompiler SetDST, GetDST, AddCodeCond, GetRawCond): CC0 and CC1 start
  at 0; an op whose result goes to neither an output nor a temp writes it to CC{sel} when the instruction updates
  condition codes (or, as RPCS3's "Far Cry 2" rule, when the other op writes the output); a conditional instruction
  writes only the components whose condition holds (`select`). 25 of the mission's programs needed it.
- Fragment DIVSQ (`_builtin_divsq`: a / sqrt(|b|), a zero numerator staying zero), PK16 and UP16 (RPCS3 packs two
  unorm16), RET (the program ends: the body runs in `do { } while (false)` and RET breaks out to the outputs), and TXL
  and TXB on cube maps.
- X16 and Y16_X16 textures (29 of the mission's textures), decoded on the GPU into R16Unorm and RG16Unorm with RPCS3's
  component mappings (X16 {R, 1, R, 1}, Y16_X16 {G, R, G, R} in A, R, G, B order).
Left for later, each logged once: vertex texture fetch (vertex TXL), floating-point and 16-bit render targets sampled
as textures and read back, multisampling (one sample), occlusion queries (65535) and conditional rendering (always
drawn).

## D-079 · 2026-10-04 · SPU modules programs load themselves; identifying images once per load

Run 049 (lifted tasks, jobs and policy module) ran the 102 lifted programs without a crash, but at the interpreted
runs' pace. Two causes, from a Time Profiler capture on the Mac (run 050):
- Hashing every load took a fifth of the workers' time: GTA V's own policy module is entered every time its workload
  is selected, and jobs and tasks start thousands of times a second. An image is now identified when it is copied into
  local store and the answer kept while the copy stays: the worker keeps its policy module's program until the module
  is reloaded, the job chain's LS cache keeps each binary's, and tasks (copied at every start) are hashed then. A DMA
  over a program's code forgets it there too. (A sampled fingerprint cache was tried first and dropped: a test showed
  it can miss a change in the middle of the code.) The SPU's floating-point mode is entered once per worker thread
  instead of on every run (about 3.5% more).
- Most of the remaining interpreted time is code the programs load themselves: GTA V's four "dependency_spu" tasks
  copy in the jobs they run as code modules (6,105 of run 049's 6,982 "no lifted code" addresses were in one such task,
  outside its own 14 KB of code). The SPU runtime now has two hooks: an observer of every GET into local store, and a
  resolver the run loop asks when execution reaches an address the attached program has no lifted code for (the
  interpreter asks on taken branches out of the attached program's range). The HLE keeps each SPU's GETs as runs of
  contiguous copies (a module comes in several 16 KB GETs), finds the run holding the address, and identifies it like
  any image: the hash of its guest copy and its LS address. A module whose local store copy differs from its guest copy
  when execution first reaches it (patched after loading) is interpreted, since its lifted code could be stale. With
  `--spu-dump`, modules are saved too, with the addresses execution entered them at as "seed" lines for the next lift;
  `scripts/lift-spu.sh` keeps only seeds inside each image (spu-lift refuses the others: the SPURS syscall stubs, code
  of other modules).
pspocket-boot logs the frame rate every 5 s, like the iOS app.

## D-080 · 2026-10-04 · Floating-point render targets read back; other formats over a target decode from memory

Run 050's first mission drew its world in pink and red. Its log named the gaps around the game's floating-point
render targets: "GPU readback of surface format 11 is not implemented" (the RSX copying a float16 target, with NV0039
or as texture memory, read stale guest memory) and textures sampling a target in another format than its own (an
A8B8G8R8 target read as Y16_X16).
- Write-back covers F_W16Z16Y16X16, F_W32Z32Y32X32 and F_X32: each component big-endian, red first, as the
  W16_Z16_Y16_X16_FLOAT, W32_Z32_Y32_X32_FLOAT and X32_FLOAT textures read them.
- A texture over a render target samples the GPU copy only when it reads it in its own format (8-bit ARGB textures
  over 32-bit 8-bit targets, as before, and the three float formats over their targets). Otherwise the target is
  written back and the texture decoded from guest memory: exact, but an expensive readback (logged). RPCS3 converts
  such reinterpretations on the GPU; that waits until the readbacks show up in a profile.
- A flush of memory under a target of the open render pass ends the pass first: a target cleared but not drawn to
  holds the clear only as the pass's load action, so the write-back read the memory from before the clear.

## D-081 · 2026-10-04 · Relocatable lifted SPU programs: one translation per job binary or module, run at any address

Run 052 (the first mission, with the modules of D-079 collected) dumped 32,570 images in a few minutes: GTA V's
dependency tasks load 82 distinct modules, each at thousands of LS addresses, and a lifted program was keyed by its
LS address (D-076). Job binaries are the same: 132 dumps were 36 programs. Code that SPURS or a program loads at
varying addresses is position independent, so it is now lifted once and runs wherever it sits.
- `spu-lift --relocatable`: the lifted code reads `ctx->reloc` (the distance from where it was lifted to where it
  runs) once per function, and every LS address it derives from its own adds it: the pc it stores, links, relative
  branch and load targets (lqr, stqr), the pc it passes to the runtime (stops, channels, halts), call returns and the
  dispatch switch. Absolute branches (bra, brasl) reach the same address wherever the code sits: they leave to the run
  loop, and the analyzer does not follow them or treat ila/il constants as entries. This is exact for any code, not
  only position-independent code: it runs as the interpreter would at that address.
- `ps_spu_program.relocatable`: found by hash and size alone; `ps_spu_attach_program_at` and the resolver say where
  the code sits. Function symbols of a relocatable lift carry an "r" before the code start.
- The context keeps a home program (the one SPURS attached) beside the current one. The resolver makes a module
  current; when execution comes back to the home program's code (the module returned), home is current again, so a
  task or job runs lifted after every module call instead of interpreting to its end. A GET over either drops it.
- The resolver returns a module's program even without an entry at pc: the miss is reported in the program's own
  terms ("no lifted code at ... in program ..."), so the logs seed the next lift as they do for other programs.
- SPURS dumps a relocatable image once per hash, where it was first seen, and module seeds are moved into that copy's
  terms; `scripts/lift-spu.sh` lifts jobs and modules with `--relocatable`, once per hash (older per-address copies
  merge into the first, with their seeds moved).
- Seeds (`spu-lift --seed`, `LsImage::seeds`) inside a function the entries reach become labels of it, entered through
  its dispatch switch; only the others start functions. Each seed used to start a function that copied all the code
  it reached: one 22 KB job, with the seeds of its 29 copies merged, lifted to 35 MB of C (367,473 instructions for
  5,536) and did not compile in 11 minutes; now 4 times its size at most, all 36 programs 53 MB.
- Tests: a quarter of the differential fuzz cases run relocated, at another address of the code window (and so do
  their relative loads); a synthetic program with calls, a loop, a call through a computed address, relative loads and
  stores, a stop, an absolute call out of it and a jump through a register runs lifted fixed and relocatable at six
  placements (one across the end of local store) against the interpreter; HLE tests for relocatable jobs, the module
  dumps and the return to the home program.

## D-082 · 2026-10-04 · Modules stay known across data copies and task switches; a lock-free commit check

Run 054 still interpreted 4,970 addresses of the modules GTA V's dependency tasks load, none of them modules changed
in local store (run 055's count: 0). Two ways the tracker of D-079 lost a module that was still there:
- Its table of GET runs replaced the oldest run, whatever it was: the tasks' data copies pushed their modules out.
  A new run now takes a free slot, else the oldest run that never ran code, else the oldest (128 slots).
- A task that yields saves its local store to its context save area and resumes from it, on any worker, by a copy the
  MFC never sees. The runs inside the saved blocks are now kept with the save area and given back when the task
  resumes; a task start forgets the runs of the image it replaced. Restored runs are verified again (guest copy
  against local store) the first time execution reaches them.
- `GuestMemory::is_committed` took the memory's mutex on every call, and SPU DMAs, SPURS and the RSX call it thousands
  of times a second from every thread: run 056's profile had the mutex at 14% of the SPURS workers' time. It now reads
  the commit bitmap with acquire loads; commit and decommit store its words atomically under the mutex.
- Tests: a module survives 300 data GETs; a task's module runs lifted after its context moves to another SPU (both
  fail without the fix).

## D-083 · 2026-10-04 · Textures over render targets read the target's bytes as the texture's format

Run 056 traced one frame of the first mission (`--trace-frame`, D-084): a deferred renderer whose lighting pass read
three of its inputs wrongly, so the HDR target stayed black and the frame showed the albedo, in the wrong colours.
- An A8R8G8B8 texture over an A8B8G8R8 target sees its bytes in the other order: red and blue trade places, as when
  the texture is decoded from the target's memory. The G-buffer is A8B8G8R8 and is read as A8R8G8B8.
- A B8 texture over a B8 target samples the target (its byte in every channel), and B8 targets write back.
- A depth buffer read as a colour texture (the lighting pass reads Z24S8 as A8R8G8B8 to rebuild positions) is written
  back to guest memory in the RSX's layout, depth in the high 24 bits and stencil in the low 8 (Z16 as halfwords),
  and the texture decodes it from there: exact, but an expensive readback each frame (RPCS3 converts on the GPU; next
  once a profile shows it).
- Tests: each of the three, end to end through the Metal backend.

## D-084 · 2026-10-04 · Debugging aids; the app recovers from FIFO errors; the app runs the lifted SPU programs

- `pspocket-boot --trace-frame N` (with `--frames`): every clear and draw of frame N is logged on channel rsx.trace
  (targets, fragment program, textures and whether each came from a target, from memory or from a written-back
  target, blending), and every colour target it used is saved as a PNG at its flip.
- A malformed FIFO command is reported with put, the words around get and the last eight jumps, calls and returns.
  Run 055 ended on one after 195 s of the first mission (`malformed FIFO command 0xb28b0bcf`); runs 050 to 054 had
  none in 15, 7, 7 and 10 minutes. The cause is not known yet.
- The app logs a FIFO error and skips to put, as RPCS3 recovers (CLAUDE.md rule 5: release builds fall back and log);
  `pspocket-boot --rsx-recover` does the same, and without it the Mac tool still stops there.
- The run loop counts interpreted SPU instructions by the program current when the interpreter took over and the
  address, and pspocket-boot prints the twelve largest at the end (`ps_spu_interp_sites`).
- `scripts/ios.sh` builds the game's lifted SPU programs into the app when it finds them ($PSPOCKET_GAME_SPU, else an
  inventory/*/spu-lifted with a game_spu.c), and the app registers them and the module resolver as pspocket-boot
  does. The lifted code comes from the user's own game and stays out of the repository (CLAUDE.md rule 1).

## D-085 · 2026-10-04 · A DMA invalidates a lifted SPU program only when it writes its instructions

Run 057's interpreter counts (D-084) showed where the SPUs' time went: 16.6 billion instructions interpreted from job
entries (LS 0x4C00) with no program attached, and 12 billion more at another job address, in 300 s. A job binary's
range is its code, data and bss, and jobs copy their buffers into their own data: D-076 dropped a program at any GET
into its range, and the job chain then marked the cached binary interpreted for good, so every job after its first run
was interpreted.
- `ps_spu_program.code_words`: one bit per word of the range, set for the words lifted as instructions; spu-lift
  writes it. A GET drops the program (and the home program, D-081) only when it writes one of those words. NULL keeps
  the old rule (the whole range), for hand-made descriptors.
- Test: a program keeps its lifted code through a GET into its data and loses it to a GET over its instructions.

## D-086 · 2026-10-04 · Vertex programs: the address register, relative constants and flow control

On the phone (run 059, the first mission) the people did not appear: every skinned draw was skipped, its vertex
program using ARL (the address register) and relative constants (bone matrices at c[A0.x + n]), which the translator
refused. RPCS3's VertexProgramDecompiler is the reference.
- ARL: a{addr_reg} = int4(floor(src0)). A relative constant reads vc[clamp(n + a{addr_reg}.{swizzle}, 0, 467)] (RPCS3
  reads past the 468 constants unchecked).
- Flow control: the body is a switch on the instruction to run next inside a bounded loop (Metal has no goto), with a
  case at every branch target and return point (at every instruction when BRA jumps to the address register). BRI and
  BRA branch when any component of their condition holds (GetCond); CAL and CLI push the return point on an 8-deep
  stack, RET pops it or ends the program. The decoder reads on past the main END to the subroutines branches reach.
- LIT (lit_legacy), EXP and LOG.
- Still refused, by name: TXL in vertex programs (vertex texture fetch), BRB and CLB (branches on the boolean
  constants).
- Tests: the translation of ARL, a relative constant, BRI, CAL and RET; Metal compiles a program with all of them.

## D-087 · 2026-10-04 · The worker's stops run as lifted stand-ins; misses re-seed the lift

Run 058 (D-085 in place) interpreted where the lift had not reached: one job binary was 36% lifted, its other code
reached through function pointers. Its misses, logged in its own terms (D-081), seeded the next lift (86% of it). And
GTA V's own policy module left through the kernel's exit at 0x808 2.1 million times a second; every time, the run loop
missed (taking the miss log's lock), interpreted one `stop` and handed back.
- The HLE's resolver gives the worker's planted stops (select 0x290, return 0x804, exit 0x808, the job return 0x2570)
  a lifted stand-in each: a 4-byte program whose entry takes the same stop, used only while local store holds that
  stop there. A wider one would have held the policy module's code too (the run loop would not have gone back to it).
- Run 059: the most interpreted address 70 million instructions in 300 s (run 057: 16.6 billion).
- Test: a policy module leaves through 0x808 without a miss, and comes back to its own lifted code.

## D-088 · 2026-10-04 · Depth buffers read as textures convert on the GPU

D-083's write-back of a depth buffer for a texture that reads it cost a full GPU stall and a CPU copy each time; GTA V
reads its main depth buffer and its shadow maps every frame (65,433 readbacks in run 058, the first mission down from 9
to 6.6 fps).
- `rsx_depth_to_texture` (compute) reads the depth buffer (and its stencil, through an X32_Stencil8 view) and writes
  what the texture's format makes of the RSX's bits: Z24S8 as big-endian words (depth high, stencil low) read as
  A8R8G8B8, D8R8G8B8 or Y16_X16; Z16 read as X16; and the depth formats (DEPTH24_D8, DEPTH16 and their float forms)
  the depth itself, in every channel. It runs in the frame's command buffer, after the passes that drew the buffer;
  the result is kept per format and redone only when the buffer was drawn to since (a generation count per surface).
- Other combinations, and sizes that differ from the buffer's, still go through the write-back.
- A colour-only clear no longer sets up the depth buffer the stale zeta registers describe: creating it replaced the
  real depth buffer at that address.
- Not yet: depth comparison (shadow tests) for the depth formats; they read the depth.
- Tests: Z24S8 as A8R8G8B8 and Z16 as X16 and DEPTH16, end to end with no readback.
- Run 060: the first mission at 22 to 25 fps on the Mac (run 059: 6 to 7).

## D-089 · 2026-10-04 · Fragment programs: IFE, LOOP, REP, BRK and the byte and half packs

Run 060 traced a mission frame whose HDR lighting target stayed black: three of its fragment programs use IFE and two
UPB, and their draws were skipped. RPCS3's FragmentProgramDecompiler is the reference.
- IFE opens `if (any(cond)) {` and closes at its end offset, switching to `} else {` at its else offset (both in SRC1
  and SRC2, words of the program, constants included). LOOP and REP open `if (cond) for (int iN = init; iN < end;
  iN += increment) {` (SRC1's counters); one whose condition never holds leaves its body in line, as RPCS3 does. BRK
  breaks the innermost loop when any component of its condition holds.
- RET sets a flag and breaks; every loop passes the flag on as it ends, so the program still reaches its outputs (the
  body runs inside do { } while (false); Metal has no goto).
- PKB/UPB (unorm 4x8), PK4/UP4 (snorm 4x8), PK2/UP2 (half 2x16).
- Still refused, by name: CAL, the loop counter as an input index, BEM, the gamma packs.
- Tests: the translation of an IFE with an else part and a LOOP with BRK and RET; Metal compiles a program with every
  new op.

## D-090 · 2026-10-04 · Any ISO, by automatic ahead-of-time translation (not an emulator)

The user's goal, in their words: the app runs "ANY ps3 iso", without making one game after another native by hand;
and pspocket is "an ahead of time translation ... not an emulator". Both hold together with D-036:
- Any game goes through the same automatic pipeline on the Mac: disc image in, the game's PPU and SPU code translated
  ahead of time to native code and compiled into a signed app, the runtime shared by all. No per-game manual work: the
  pipeline is one command (still to be written as such; its parts exist). iOS does not let an app create native code
  after signing, so the translation cannot run on the phone itself.
- The runtime reads disc images directly and has no title checks (CLAUDE.md rule 6): lv2, the HLE modules, SPURS and
  the RSX to Metal translator serve every game.
- The interpreters are the oracle and the safety net, not the path: code the translation has not found yet (code a
  game only reaches at run time) is interpreted, logged, and translated by the next build automatically (the seeds of
  D-081 and D-087). Their speed matters only for that (a decode cache for the PPU interpreter, D-091, is cheap).
- Next for speed: the PPU, still interpreted for GTA V (about two cores in run 060's mission), translated ahead of time
  like the SPUs.
- Fixes are judged by whether they help every game; GTA V remains the game they are measured with.

## D-091 · 2026-10-04 · A decode cache in the PPU interpreter

decode()'s switch on the primary opcode was a quarter of the PPU threads' time in run 060 (an indirect branch the host
mispredicts on nearly every instruction). The interpreter now keeps one 64-bit word per address (by pc bits 2-17): the
instruction word, its op and whether it branches. The op is a function of the word alone, so an entry is used exactly
when its word matches memory; code loaded or patched since simply misses. Shared by the PPU threads, read and written
relaxed (a race is a miss). Test: code patched at an address after it ran runs as patched.
Run 062: the first mission at 26 to 28 fps (run 061: 22 to 24).

## D-092 · 2026-10-04 · The game's PPU code translated ahead of time, built in

`ppu-lift` translates GTA V's whole executable (both loaded segments as one image from 0x10000, the entry and the 52,704
.opd descriptors as seeds) in 6.3 s: 85,342 functions, 6,086,213 instructions, no fallbacks, 823 MB of C in 512 units
(`--name game_ppu --prefix game_ppu_`, so the names say nothing about the game). One unit compiles in about 1.6 s.
- `-DPSPOCKET_GAME_PPU=DIR` builds that output into pspocket-boot without debug information; pspocket-boot registers
  it in the PPU dispatch table at start. The interpreter enters a translated function wherever a branch reaches one
  (D-028's interop), and translated code falls back to the interpreter for what it cannot reach.
- The HLE's import trampolines live in their own memory, so they and the game's functions never share an address.
- Like the SPU programs, the output is derived from the user's game: it stays in inventory/ (CLAUDE.md rule 1).

## D-093 · 2026-10-04 · Vertex texture fetch; skipped draws in the frame trace

The frame trace now names skipped draws and why (D-084). Run 063's mission frame skipped five, all for their vertex
programs: the post-processing chain (into the target the final composite shows, the HDR downsample, a bloom target and
the 1x1 exposure value), whose vertex programs read the exposure through TXL. That is why the frame showed the G-buffer
albedo instead of the lit image.
- TXL in a vertex program samples vertex texture unit `tex_num` at level 0 with src0.xy (RPCS3
  VERTEX_TEXTURE_FETCH2D: textureLod($t, $0.xy, 0)); the four units are bound as vertex textures and samplers.
- Tests: the translation, and a draw whose vertex colour comes from a vertex texture, end to end.

## D-094 · 2026-10-05 · Translated functions start at their entry; SNC switches; locating faults in translated code

Run 065 (GTA V with its PPU code translated, D-092) faulted on a small guest address within seconds, not always at the
same point.
- The cause: the lifter emitted a function's instructions in address order and started at the lowest one. SNC emits
  wrappers that set up an argument and then branch back into a shared body below them (here an mbrtowc-like function:
  `addis r7, r13, 0; addi r7, r7, -28212; b body`); the translated wrapper started in the body with r7 unset. A
  function whose entry is not its lowest instruction now begins with a goto to its entry. 9 of GTA V's 85,370
  functions have that shape. Test: such a function, translated and interpreted, gives the same result.
- SNC's switch shape is recognised: the bounds check compares a masked copy of the index (`clrlwi r0, r9, 24; cmplwi
  cr7, r0, 83`), the scaling masks too (`rlwinm r9, r9, 2, 22, 29` is (r9 & 0xFF) * 4), the table address is a word
  loaded from the TOC, and the entries are sign-extended with extsw after lwzx. The bounds check is accepted only when
  it is the instruction that sets the branch's CR field, sees every index bit the scaling uses, and the index does not
  change between them. GTA V: 9 tables recognised (none before); the game has few bctr switches.
- Finding the function: a fault now prints the host program counter and frame-pointer chain with the image slide, so
  `atos -o pspocket-boot -s SLIDE ADDR...` names the translated functions (game_ppu_f_<address>) on the stack. The guest
  cia is stale in translated code. PSPOCKET_PPU_ONLY=LO-HI[,LO-HI] keeps only those translated functions, for
  bisecting; the fault here was intermittent, which misled a bisection, so the host frames are the tool to reach for.
- ppu-dis disassembles a raw image (`ppu-dis --image FILE --base ADDR ADDR [COUNT]`); PSPOCKET_PPU_TRACE_MISSES=N
  prints the registers around the first N interpreter fallbacks of translated code.

## D-095 · 2026-10-05 · Modules found in a shared 1 KB block; the task syscall stop lifted

Run 066 (GTA V with its PPU code translated) interpreted 201 million SPU instructions at LS 0x071f0 in one dependency
task (spu_5d53598703d84caa) and 7,596 other addresses of the modules it loads, while the task's syscalls missed at
0xA70 every time.
- The module tracker finds the run holding pc through the 1 KB block it is in, one run per block. The task copies a
  module to 0x71f0 (0x110 bytes) and then its data to 0x7310, in the same block: the data's run took the block and the
  module could not be found. When the block's run does not hold pc, the resolver now looks through all the runs.
  Test: a module at 0x71f0 with data copied to 0x7310 runs lifted (fails without the fix).
- The taskset's syscall entry (0xA70, a `stop` the worker catches) gets a lifted stand-in like the kernel's stops
  (D-087): each syscall was a miss, a log lookup under a lock and one interpreted instruction. Test: a task's syscall
  stops without a miss.
- PSPOCKET_SPU_TRACE_RUNS=N prints, for the first N addresses the resolver finds no run for, the tracker's last
  events over that block (GETs, runs they cleared, forgets, restores): how the shared block showed.

## D-096 · 2026-10-05 · Jump tables whose base the prologue sets; dispatch across a call

GTA V's script VM (0x91bf00) is a threaded bytecode interpreter: every handler ends in its own dispatch, `lbz r3,
1(r3); slwi r3, r3, 2; lwzx r3, r19, r3; mtctr r3; bctr`, with r19 set once in the prologue (`lis r19, 434; addic r19,
r19, -21596`). Only the first dispatch was within the matcher's backward slice; at the second the translated VM fell
back to the interpreter until it returned, so scripts ran interpreted.
- The prefix's constants: the non-volatile registers (r14 to r31) the entry's straight-line code sets to a constant
  (lis, addis, addi, addic, ori, oris, mr, word and doubleword loads from the image; calls keep them) and nothing else
  in the function writes, except on the way out (writes that run straight into a return without a branch or call:
  SNC's functions branch to shared helpers that restore the saved registers, which become part of the function). The
  matcher uses that value where its slice finds no definition, after the prefix's last write of the register. The
  function is explored again with those constants until they hold for all the code it then reaches.
- `addic` evaluates like addi (SNC builds addresses with it), and an index loaded as a byte (or masked by andi. or
  clrlwi) caps a table without a bounds check at the values it can take: the VM's 256, of which 226 entries are code.
- A bctr still unrecognised in a function with jump tables switches over the targets those tables proved before
  falling back: three VM dispatches load the next handler's address before a call and jump after it.
- A function's table sites share one dispatch switch at its end (`jt_site = pc; goto D_tables;`, and `goto D_known;`
  for the unrecognised ones): with a switch of its own at each of the VM's 124 sites, clang worked on that one unit for
  over 20 minutes; shared, 1.7 s.
- GTA V: 133 jump tables (10 before), the VM's 124 dispatch sites among them.
- Tests: a synthetic VM with its table base set in the prologue and restored by a helper it branches to, a call
  between the prologue and the first dispatch, byte opcodes and a dispatch across a call, run translated against the
  interpreter with no fallback; the analyzer finds its three tables (each fails without its part of the change).

## D-097 · 2026-10-05 · DEPTH16 textures decoded from memory

Run 066 logged 35 DEPTH16 textures the backend could not decode (GTA V's shadow maps, read where the surface cache
has no depth buffer for them), and the RSX counted 532,716 failures in 200 s, most of them these. DEPTH16 now decodes
from memory as 16-bit unorm (TexelDecode::D16, R16Unorm) and reads the depth in every channel, as RPCS3's component
mapping does for the depth formats and as D-088's conversion does for a depth buffer. The float and 24-bit depth
formats stay unsupported until a game reads them from memory. Tests: the CPU and GPU decoders agree, and the channel
mapping.

## D-098 · 2026-10-05 · Colour targets read as Y16_X16 convert on the GPU; translated code only for its executable

- Run 066 wrote two 640x360 A8B8G8R8 targets back to guest memory 4,440 times each (every frame), because a pass of
  the first mission reads them as Y16_X16 textures: a GPU sync and a CPU decode each time. `rsx_color_to_texture`
  (compute) now makes the texture from the target's bytes on the GPU, as RPCS3 converts, when the texture's texels
  are the target's pixels (a 4-byte target, same size and pitch, linear) and the draw does not also render to it; kept
  per generation of the target like D-088's depth conversions. Run 068: none of those readbacks. Test: an A8B8G8R8
  target read as Y16_X16, end to end, with no readback (fails without the conversion).
- A readback's log line names what asked for it: a texture of another format, an NV0039 buffer copy or an NV3089
  image copy. What remains in run 068 is one 80x45 target the game copies out with NV0039 every frame.
- The game's translated PPU code built into pspocket-boot (D-092) is registered only when the executable is the one it
  was translated from: ppu-lift writes the words found at 64 of the functions' entries (`N_image_words`), and the host
  checks them in guest memory after the load. The boot tests' synthetic executables, at the same addresses, ran GTA
  V's functions and faulted.

## D-099 · 2026-10-05 · The module tracker keeps every run until a GET overwrites it; SPU seeds accumulate

Run 068 still interpreted 3.9 million SPU instructions of the dependency tasks' modules: a task copies a module in,
then hundreds of small pieces of data, and only then runs the module. With 128 runs per SPU, replaced oldest first
among those that had not run yet, the module's run was gone (PSPOCKET_SPU_TRACE_RUNS showed its GET as the last event
over its block).
- A run now lives until a GET overwrites part of it (4,096 per SPU, more than local store holds in practice). Each
  1 KB block of local store lists the runs that touch it (up to 32; a block of smaller pieces drops its oldest), so a
  GET only looks at the runs it may overwrite and the resolver only at the runs in pc's block: the old table scanned
  all 128 runs on every GET. Forgetting walks the blocks, not the 4,096 slots, as tasks start thousands of times a
  second.
- Run 069: none of the dependency tasks' code interpreted; four addresses of one job binary left (function pointers).
- scripts/lift-spu.sh keeps the entry points of the programs an earlier lift made as seeds: the logs that gave the
  seeds are not kept, and a later lift without them would have lost that code.
- Test: a module copied in, then 300 pieces of data, then run: lifted (fails with the old table).

## D-100 · 2026-10-05 · Shadow-map lookups compare depth; atlas tiles are separate surfaces

- A depth-format texture (DEPTH16, DEPTH24_D8 and their float forms) with a compare function other than never and
  always, and no alpha kill, is a shadow lookup (RPCS3 RSXThread.cpp shadow_textures): TEX and TXP return the
  comparison of the coordinate's z (TXP: z / w) against the depth, in every channel, instead of the depth. The
  translator has a Shadow2D texture kind; `rsx_shadow` compares the texel at uv, or with linear filtering the 2x2
  texels around it and blends them bilinearly (percentage-closer filtering, as RPCS3's comparison samplers do); the
  compare function and filtering come in the unit's tex_scale.zw, so they do not multiply the shaders. The reference
  saturates at 1 for the integer depth formats (RPCS3 _texcoord_xform_shadow).
- Run 070's frame trace showed GTA V's shadow cascades as 128x128 Z16 tiles of one depth atlas (zeta offsets 0x100
  apart, pitch 0x400): each tile's surface replaced the one before, since their bytes interleave, and the cascades were
  read from stale memory. Surfaces that share a pitch now overlap only where their pixel rectangles do (rows that wrap
  past the pitch still compare bytes). Run 071: every DEPTH16 texture of the traced frame is a converted depth buffer.
- Tests: a shadow lookup with nearest and linear filtering, end to end (the blended values at the edge); the translation;
  two depth tiles of one atlas cleared to different depths and read as textures (fails with the byte overlap).

## D-101 · 2026-10-05 · One command per game: ppu-lift reads the executable; scripts/translate-game.sh

D-090's pipeline as commands a user runs once per game, with nothing found by hand:
- `ppu-lift --elf EBOOT.elf` takes the decrypted executable: its LOAD segments (code only where PF_X, bss as zeros),
  the entry and every function descriptor of its .opd as seeds with their TOCs (recomp/common's analyze_ppu_module
  finds the .opd by section or probe, as ps3inspect does), and the entry's TOC for the rest. For GTA V the output is
  byte for byte the lift made from the hand-made raw image, its .opd range and TOC (D-092).
- `scripts/translate-game.sh --iso GAME.iso --elf EBOOT.elf [--seconds S] [--press ...] [--ios]`: the title ID from
  the disc names inventory/<TITLE_ID>/ (gitignored); ppu-lift --elf; pspocket-boot built with the PPU code; one run
  with --spu-dump to collect the SPU programs; scripts/lift-spu.sh; pspocket-boot rebuilt with both; with --ios the
  app too. Running it again covers more of the game (the SPU seeds accumulate, D-099), and what is not translated yet
  runs in the interpreters meanwhile.
- Decrypting EBOOT.BIN stays with the user's own tools (the keys never enter the repository or the app); the script
  refuses a SELF.
- The iOS app builds the translated PPU code in when scripts/ios.sh finds it ($PSPOCKET_GAME_PPU, else an
  inventory/*/ppu-lifted) and registers it after the load for its executable only (D-098), through
  loader::register_translated_ppu, as pspocket-boot does.

## D-102 · 2026-10-05 · Occlusion queries, conditional rendering and 3D textures

Run 072's RSX counted 360,000 failures in 170 s; run 073 counts one.
- Occlusion queries: NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE turns counting on and off, CLEAR_REPORT_VALUE(ZPASS_PIXEL_CNT)
  restarts it, GET_REPORT(ZPASS_PIXEL_CNT) asks for the samples that passed since (RPCS3 ZCULL_control). The Metal
  backend gives each render buffer a visibility buffer of 64-bit counters and each query one counter per render pass it
  draws in (Metal writes a pass's counters when the pass ends); a report sums its counters in a completion handler of
  the render buffer that holds the last of them and stores the value word, without a GPU sync. The front end writes
  the report's timestamp at once. Backends that do not count still answer Config::fake_zpass_count.
- Conditional rendering (SET_RENDER_ENABLE mode 2) reads the report's value when the method runs and skips draws while
  it is zero, until mode 1: a query issued in the same frame decides from its previous result, a frame late (RPCS3
  defers the evaluation; Metal has no predication). The thread report counts the skipped draws.
- 3D textures: a level is its slices one after another (linear), or swizzled with x, y and z bits interleaved over the
  power-of-two volume (RPCS3 get_subresources_layout_impl, convert_linear_swizzle_3d, calculate_z_index), decoded on
  the GPU by rsx_decode_texture3d (all slices in one dispatch, the texel decoding shared with the 2D kernel) into a
  Metal 3D texture, sampled with coord.xyz (TXP: / w) through a Tex3D texture kind. GTA V samples a 64x64x32 R5G6B5
  swizzled volume. Compressed 3D textures (VTC tiling) stay unsupported until a game uses them.
- Multisampled surfaces still render with one sample: GTA V's one (AA mode 3, 128x128) is read by its resolve at the
  sample layout, twice as wide, which samples the stretched target and gets the pixel itself. Reported once per
  surface instead of at every draw.
- Tests: an occlusion count equal to the pixels the triangle covered, a query without draws reporting 0, a draw under a
  zero report skipped and one under a nonzero report drawn; 3D layouts, the 3D Morton order and the CPU decoder; a
  swizzled 3D texture sampled end to end at two slices.

## D-103 · 2026-10-05 · SPU workers back off a policy module that keeps finding no work

At the 30 fps cap the Mac process used 4.8 cores, five threads at 85 to 88% each. Each SPURS worker now reports where
its time went (`ps_spu`'s new per-context count of main-memory writes tells a run that changed nothing from work):
four of GTA V's five Primary SPUs spent 30 to 43 s of a 140 s run in 120 to 190 million runs of the game's own
policy module that found no work, two million a second each, and 0.1 s waiting. The PS3's SPUs spin like that; here
each spin takes a core the PPU threads need, two of the iPhone's six.
- After 64 empty polls in a row the worker sleeps 5 us, doubling up to 100 us (the longest new work then waits), until
  a run writes memory again. Waiting for the line the module polls to change instead woke at once: the other SPUs take
  jobs from the same queue (4 s of waits in 140 s, the polls hardly fewer).
- Run 074 (same scene, 140 s): 2.6 cores instead of 4.8 at the same 30 fps; 3.9 million empty polls per SPU instead of
  120 to 190 million, 1.2 to 1.4 s instead of 28 to 43 s.

## Open decisions (plan section 14)

- [x] Licence → D-001.
- [x] Game version → D-025: GTA V BLES01807 (GTA IV in D-002 stays as a draft).
- [x] Toolchain → D-012 (write our own).
- [x] SPURS → D-037: native (HLE module + host scheduler). libsre from the user's firmware is a reference only, for job
  chains and queues.
- [~] Target devices: primary test device is D-011; minimum chip and the iPad question are still open.
- [ ] Input: controller required, or a polished touch overlay as well.
- [x] ISO handling → D-043 (read directly from the ISO).
