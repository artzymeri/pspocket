# PS3 to iOS Static Recompiler: GTA IV Port Plan

Oct 3, 2026 · @Frrinci

## 1. Goal and scope

Build a per-game static recompiler that turns a PS3 game's code into a native, signed iOS app, starting with GTA IV.
Target: a locked 30 fps at native 1152×640, upscaled with MetalFX, on A18 Pro-class iPhones and M-series iPads.

The method in one line: game code is translated to C on a Mac ahead of time and compiled into the app. At runtime the
app only reads assets from the user's own ISO.

### Goals

- PPU and SPU code recompiled ahead of time to ARM64. No JIT, and no interpreter on the hot path.
- A runtime that reimplements the PS3 kernel (lv2), the firmware modules the game imports, SPU execution, and RSX to
  Metal rendering.
- One iOS app per game: pick ISO, verify hash, extract assets, play.
- A shared toolchain and runtime, so a second game costs a fraction of the first.

### Non-goals

- A generic PS3 emulator or a "load any ISO" app.
- On-device recompilation or JIT. iOS code signing forbids running native code that wasn't signed at build time.
- Online play, trophies, PSN features.
- Distributing game code, assets or keys in any form.

### Honest framing

- Nobody has shipped a statically recompiled PS3 game yet. This is new ground.
- GTA IV is a hard first target: a large multithreaded engine, heavy streaming, SPU jobs. The plan proves the pipeline
  on homebrew and test programs before touching it.
- Estimate for one strong engineer plus Claude, part-time: 1.5–3 years to stable and playable, with a real chance of
  stalling.
- If the only goal were GTA IV on a phone, the PC version through an x86 to ARM64 and D3D9 to Metal stack is a much
  smaller project. This plan assumes the goal is the PS3 pipeline itself.

## 2. Hard constraints

These are fixed by the platform and the law. Every design choice below follows from them.

### iOS platform

- **No JIT.** All CPU code must be compiled into the signed app. Code the build didn't capture can only run in a slow
  interpreter fallback.
- **GPU code is the exception.** Compiling Metal shaders at runtime is allowed, so shader translation can happen on
  device, backed by a precompiled cache.
- **Memory.** The runtime reserves a 4 GB guest address space. That needs the
  `com.apple.developer.kernel.extended-virtual-addressing` and `com.apple.developer.kernel.increased-memory-limit`
  entitlements, which need a paid Apple Developer account (verify current requirements).
- **Thermals.** Sustained load throttles iPhones after several minutes. Budget the worst-case frame at about 22 ms of
  the 33.3 ms limit.
- **Threads.** iPhone Pro chips have 2 performance and 4 efficiency cores. Thread affinity can't be set; use QoS
  classes instead.
- **Metal only.** No Vulkan, no MoltenVK.

### Legal

- The repo contains no Sony or Rockstar code, keys, firmware or assets. `.gitignore` blocks `*.iso`, `*.elf`,
  `*.self`, `*.sprx`, `EBOOT.BIN`, `*.PUP` and `keys/`.
- The user supplies their own decrypted dump. Builds stay private: Take-Two actively takes down GTA reverse-engineering
  projects.
- RPCS3 is GPLv2. Reading it as reference is fine; copying its code makes this project GPLv2. Decide before Phase 1
  (section 14).

### Development environment

- The toolchain runs on macOS (Apple Silicon) and Linux.
- The runtime is developed macOS-first: same ARM64 ISA and Metal API as iOS, no signing friction, better debugging. A
  minimal iOS test host arrives in Phase 4 so device performance is visible early; the full app comes in Phase 7.
- Device work (Xcode on device, Instruments traces, thermal runs) needs the user's iPhone. Claude Code can't run it;
  the user runs it and reports results.

## 3. Architecture

Three parts: an offline toolchain that runs once per game on the Mac, a runtime library linked into the app, and a thin
iOS app shell. Everything down to the Xcode build happens once per game on the Mac; the phone never translates CPU code.

### Offline toolchain (`recomp/`, Mac or Linux)

1. **Loader:** reads the decrypted EBOOT ELF and PRX modules, maps segments, parses imports and exports by NID.
2. **PPU analyzer and lifter:** finds functions, jump tables and indirect-call targets; emits one C function per guest
   function.
3. **SPU extractor and lifter:** pulls SPU programs from the ELF and from runtime traces; emits C per program.
4. **Game manifest:** ISO hash, function overrides, patches, SPU replacement table, shader and pipeline list.

Output: a generated C source tree per game, compiled by Xcode together with the runtime into a signed app.

### Runtime (`runtime/`, C++20, linked into the app)

Guest memory, PPU thread runtime, lv2 syscalls, firmware module HLE, SPU runtime, RSX to Metal renderer, audio, input,
filesystem.

### App shell (`app/ios/`, Swift and SwiftUI)

ISO picker, hash check, asset extraction, settings, controller support, save management.

**Data flow:** decrypted EBOOT → toolchain → generated C → Xcode → signed app. On device: user's ISO → hash check →
extracted assets → runtime reads them through the `/dev_bdvd` mapping.

## 4. Guest memory model

Guest addresses are 32-bit, and every access is `host_base + (uint32_t)addr` with an explicit byte swap. This one rule
keeps lifted code simple and fast.

- **Reservation:** reserve 4 GB of virtual address space at startup (mmap with `PROT_NONE`), then commit regions as the
  guest maps them. Put guard pages after the region so a stray offset faults instead of corrupting host memory.
- **Memory map:** take the exact guest layout (main memory, user allocations, RSX local memory, stacks) from RPCS3's VM
  implementation. Don't guess addresses.
- **Endianness:** `read_be<T>(addr)` and `write_be<T>(addr, v)` built on `__builtin_bswap*`. ARM64 swaps in one
  instruction (REV), so the cost is small.
- **Vectors:** VMX and SPU loads use 16-byte aligned accesses plus a byte reverse. One lane-order convention for the
  whole project, documented in `docs/decisions.md` (see section 5).
- **RSX local memory** (256 MB) lives in the same guest space. The renderer keeps GPU-side copies; CPU writes to texture
  or vertex memory must invalidate them.
- **Write tracking for the renderer:** write-protect pages that back GPU resources with `mprotect`; on the fault, mark
  the resource dirty, unprotect the page, resume. Fallback if fault handling proves unreliable on iOS: hash resources
  when bound.
- **Debugger note:** LLDB stops on these intentional faults. Configure it to pass SIGSEGV and SIGBUS through to the app
  (`process handle -p true -s false`).
- **No general fastmem:** guest-to-host translation is a plain add, so faults are only used for write tracking.

## 5. PPU recompiler

The lifter turns every reachable guest function into a C function `void f_XXXXXXXX(PPUContext* ctx)`, in the style of
XenonRecomp. A PPU interpreter is built first: it is the test oracle and the runtime fallback.

### Discovery

- Seeds: ELF entry point, `.opd` function descriptors (address and TOC), exports, import stubs, and code addresses found
  in jump tables and data.
- Per-function control-flow analysis: basic blocks, branch targets, `bctr` jump tables (match the compiler's table
  patterns), tail calls.
- Indirect calls (`bctrl` through function pointers and vtables): a global dispatch table maps guest address to host
  function. Every discovered function is registered in it.
- Unknown indirect target at runtime: log the address and abort in dev builds; fall back to the interpreter in release
  builds. Add the address as a seed for the next build.

### Context and code generation

- `PPUContext`: 32 GPRs (64-bit), 32 FPRs, 32 VRs (128-bit), CR as 8 fields, LR, CTR, XER (SO, OV, CA), FPSCR, VSCR,
  reservation state.
- Load context registers into locals at function entry; spill only around calls and syscalls, so clang keeps them in
  host registers.
- Emit plain C and let clang -O2 optimize. Split output into many translation units for parallel builds.

### Instructions that need care

- **Integer:** carry and overflow (`addc`, `adde`, `subfe`, XER[CA]), record forms updating CR0, `rlwinm` and `rldic*`
  masks, 32-bit versus 64-bit results.
- **Floating point:** `frsp` and single-precision ops round to float; `fmadd` and friends must use `fma()` for exact
  results; model FPSCR bits only if the game reads them.
- **VMX (AltiVec) to NEON:** pick one lane-order convention and implement `vperm`, `lvsl`, `lvsr` and `vsldoi` against
  it. XenonRecomp solved the same problem for VMX128; read its approach first.
- `lwarx`, `stwcx.`, `ldarx`, `stdcx.`: go through the reservation table shared with the SPUs (section 6).
- `sync`, `lwsync`, `eieio`, `isync`: map to `atomic_thread_fence` or `dmb`.
- `mftb`: host monotonic clock scaled to the PS3 timebase (79.8 MHz).
- `sc`: lv2 syscall dispatch (section 8).

### Testing

- Per-instruction differential fuzzing: random register states, run lifted C and the interpreter, compare the whole
  context. Every supported instruction gets a test.
- Resolve disputed semantics against RPCS3's PPU interpreter, and run the PPU tests from ps3autotests where they apply.

## 6. SPU recompiler and runtime

Most of the project's risk sits here. The SPUs carry the game's parallel workload, and their sync semantics are easy to
get subtly wrong.

### Finding SPU code

- **Static:** scan the PPU ELF for embedded SPU ELF images (`e_machine = 23`, `EM_SPU`) and extract them.
- **Runtime:** hook `sys_spu_image_open`, `sys_spu_thread_initialize`, `sys_spu_thread_group_start`, raw SPU loads, and
  SPURS taskset and job submission. Log a hash of every program and overlay loaded into local store.
- Every new hash a trace run finds goes into the manifest, gets lifted, and is compiled into the next build.

### Lifting

- One C function per SPU program entry, keyed by a hash of the code region. The 128 registers of 128 bits map to NEON
  types (`uint32x4_t`, `float32x4_t`).
- Indirect branches (`bi`, `bisl`): dispatch table of local-store addresses within that program.
- Overlays and code DMA'd into local store at runtime: look up compiled code by hash of the loaded region. Unknown hash
  → SPU interpreter fallback and a log line.
- **Single precision:** no denormals, NaN or infinity, rounds toward zero. Set FPCR on SPU threads to flush-to-zero and
  round-toward-zero. Model extended range (saturate instead of infinity) only behind an accuracy flag.
- **Double precision** is IEEE with round-to-nearest. Because FPCR rounding applies to both widths on ARM, emit DP ops
  with explicit rounding or a temporary FPCR switch.

### Local store and MFC

- Local store: 256 KB per SPU thread. Addresses wrap at 256 KB; quadword accesses mask to 16-byte alignment.
- DMA (GET, PUT, the list forms, fences, barriers): `memcpy` between local store and guest memory, executed
  synchronously at issue. Tag completion is then immediate.
- Channels: inbound, outbound and interrupt mailboxes, signal notification, decrementer, event mask and status → host
  waits on condition variables. A read on an empty channel blocks the host thread.

### Atomics: one shared reservation system

- GETLLAR, PUTLLC and PUTLLUC on the SPUs, and `lwarx`/`stwcx.` on the PPU, all reserve 128-byte lines. One shared
  table: a version counter per line plus striped locks.
- PUTLLC: lock the line, compare its 128 bytes with the GETLLAR snapshot, write if equal, bump the version.
- Reservation-lost event (SPURS and cellSync sleep on it): wait for the line's version to change instead of spinning.
- Heaviest stress testing in the project: multithreaded litmus tests (mutexes, queues built from these primitives)
  running for hours with zero lost updates.

### Threading

- SPU thread groups → host threads, created up front. QoS: PPU main thread and RSX thread userInteractive; SPU threads
  userInitiated.
- Busy-wait loops in SPU code waste a core. Detect common polling patterns and turn them into waits.

### Middleware replacement (main performance lever)

- After profiling, replace hot, well-known SPU programs by hash with native code. Candidates: the SPURS kernel, Edge
  zlib decompression, Edge geometry (to Metal compute), MLAA, audio decoders.
- Replace only what the Phase 0 inventory and profiling justify. Each replacement gets an A/B test against the lifted
  version on identical inputs.

## 7. RSX to Metal renderer

Games write raw RSX command buffers, so the renderer is a command-stream interpreter with a Metal backend, not an API
wrapper.

### Command FIFO

- The game writes commands into a buffer and advances the put pointer in the gcm control register. A dedicated RSX
  thread consumes from get to put.
- Decode NV4097 (3D class) methods plus JUMP, CALL, RETURN and NOP. Honour semaphores, labels and report writes
  exactly: the game uses them to sync with the GPU.
- gcm macros are inlined into game code, so hooking `cellGcm*` functions is not enough. The FIFO parser is required.
- Flip commands present the current display buffer.

### Shaders

- Vertex programs: microcode uploaded through methods into vertex program memory. Fragment programs: microcode in local
  or main memory, with constants embedded in the instruction stream that games patch per draw.
- Hash programs with embedded constants masked out; pass the constants in a buffer.
- Translate to MSL. Use RPCS3's shader decompilers as the reference for semantics (licence permitting).
- Compile with `newLibraryWithSource` and cache pipelines in an `MTLBinaryArchive`. During dev playthroughs, record
  every shader and pipeline, then ship the archive in the app so players never compile mid-game.

### Pipeline state

- Track RSX render state and build Metal pipeline state objects keyed by vertex program, fragment program, vertex
  layout, blend, depth-stencil, raster state and render-target formats. Cache aggressively.

### Surfaces (render targets)

- Surface cache keyed by address, format, pitch and size. Render targets stay on the GPU.
- When the game samples a render target as a texture (address overlap), bind the Metal texture directly.
- CPU reads of render-target memory force a GPU-to-CPU sync, which is very slow on Apple's tile-based GPUs. Log every
  occurrence in GTA IV, then patch it out or move the consumer to the GPU.

### Textures

- Formats: ARGB8, DXT1/3/5 (BC1–3 in Metal), 16-bit formats, swizzled and linear layouts, the channel remap register.
- Decode, unswizzle and byte-swap in a Metal compute pass, not on the CPU. Invalidate through the write tracking in
  section 4.

### Vertex and index data

- Vertex attributes are big-endian. Use vertex pulling: the MSL vertex shader reads raw buffers and byte-swaps, so the
  CPU never converts vertex data.
- Big-endian 16- and 32-bit index buffers: same approach, or a compute conversion pass.

### Tile-based GPU rules

- Set load and store actions correctly per render pass, avoid needless render-target switches mid-frame, and use
  memoryless depth where the game never reads it back.

### Output and pacing

- Render at native 1152×640 and upscale with MetalFX. Internal resolution scale as an option.
- Pace frames with `CAMetalDisplayLink` (or `CADisplayLink`) locked to 30 Hz.

## 8. lv2 kernel and firmware modules

Implement only what GTA IV imports. The Phase 0 import inventory is the to-do list; anything not on it is out of scope.

### lv2 syscalls (host code)

- Threads: `sys_ppu_thread_*` → pthreads.
- Sync: `sys_mutex`, `sys_cond`, `sys_semaphore`, `sys_rwlock`, `sys_event_queue` and `sys_event_port`,
  `sys_event_flag`, `sys_lwmutex` and `sys_lwcond`. The lightweight primitives are partly user-space library code;
  check what the game actually links.
- Memory: `sys_memory_*`, `sys_mmapper_*`.
- SPU: `sys_spu_*`, `sys_raw_spu_*` → SPU runtime.
- Time: `sys_time_*`, `sys_timer_*`.
- Filesystem: `sys_fs_*` → host files.
- PRX: `sys_prx_*`. Modules are linked at build time, so a load resolves to precompiled code.

### Firmware modules (resolved by NID)

- Expect at least cellGcmSys, cellSysutil, cellVideoOut, cellResc, cellFs, cellPad, cellAudio, cellSpurs, cellSync,
  cellSysmodule, cellGame, cellSaveData and codec modules. The real list comes from the inventory.
- Each import resolves at build time to a host implementation, or to a stub that logs
  `UNIMPLEMENTED module::function (NID)` and returns an error code.
- Modules that are themselves PPU or SPU code (SPURS internals, for example): choose per module between HLE (rewrite
  natively) and LLE (recompile it like game code). LLE needs the user's own firmware file, which stays out of the repo.

### Filesystem mapping

- `/dev_bdvd/` → extracted ISO in the app container, read-only.
- `/dev_hdd0/game/<TITLEID>/` and save data → app Documents.
- `/dev_hdd1/` cache → app Caches.

## 9. iOS app shell

The app is thin; everything heavy lives in the runtime. One app per game, built from that game's manifest.

### First launch

1. `UIDocumentPickerViewController` → the user picks the ISO or a folder dump.
2. Stream the file through SHA-256 and compare with the manifest hash. Wrong version → a clear error naming the
   expected title ID and region.
3. Reject encrypted (Redump-format) images unless a disc key is supplied. The expected input is a decrypted dump.
4. Extract `PS3_GAME/USRDIR` and the other needed files to the app container, with progress and a free-space check.
   Option: read straight from the ISO through an ISO9660 reader to save space.
5. Store a marker so later launches go straight to the game.

### Runtime host

- A single Metal view. Guest threads run on their own; the main thread only handles input and lifecycle.
- Backgrounding: pause guest threads and flush saves. Memory warning: drop shader and texture caches first.

### Input

- GCController (DualSense, Xbox, MFi) → cellPad. Configurable touch overlay as a fallback.

### Saves

- cellSaveData → Documents, visible in the Files app for backup.

### Signing

- Entitlements from section 2, signed with the user's own developer account. Private install only, no App Store
  submission.

## 10. Roadmap

Eight phases, about 19–31 months in total, each ending in a gate that must pass before the next starts. Homebrew and
test programs act as the pilot, so GTA IV only enters once the pipeline is proven. A phase starts only when the
previous gate passes.

### Phase 0: Inventory and go/no-go (2–4 weeks)

- [x] User decrypts EBOOT.BIN and the game's PRX files with RPCS3's binary decryption utility. (GTA V EBOOT,
  decrypted with RPCS3 `--decrypt`, D-022; GTA V has no PRX files.)
- [x] Build `tools/ps3inspect`: ELF and PRX headers and segments; imports grouped by module with NIDs resolved to
  names; exports; embedded SPU images with counts, sizes and hashes; strings that identify middleware.
- [ ] Trace a 30-minute desktop session in RPCS3, including driving: SPU programs loaded (with hashes), syscalls used,
  render-target readbacks, shader count. This may need a locally patched RPCS3 build with extra logging.
- [x] Evaluate PS3Recomp: licence, maturity, output quality on the GTA IV ELF. (Done by static reading, see
  `docs/phase0-recomp-evaluation.md`; the GTA IV run waits for a dump.)

**Exit:** an inventory report with function count, imported NIDs per module, and SPU programs split into Sony
middleware versus custom. Go/no-go decision and a re-estimate.

### Phase 1: Toolchain foundation (2–3 months)

- [ ] ELF/PRX loader, NID database, manifest format.
- [ ] PPU interpreter (oracle).
- [ ] PPU analyzer and lifter, with differential fuzz tests for every supported instruction.

**Exit:** PSL1GHT samples recompile and match the interpreter exactly. The GTA IV ELF lifts with no unsupported
instructions.

### Phase 2: Runtime core on macOS (2–3 months)

- [ ] Guest memory, PPU threads, lv2 sync, memory, time and filesystem syscalls, module stub generation, structured
  logging.
- [ ] Run PSL1GHT console samples and the PPU and lv2 tests from ps3autotests natively.

**Exit:** the test suite passes on macOS. The recompiled GTA IV EBOOT starts, initializes, and stops at its first
unimplemented function with a clear log line.

### Phase 3: SPU (3–4 months)

- [ ] SPU interpreter (oracle), lifter, local store, MFC, channels, thread groups.
- [ ] Shared reservation system with litmus stress tests.
- [ ] SPURS: HLE or LLE, per the decision in section 14.

**Exit:** SPU tests pass, multi-hour stress runs show zero lost updates, and every SPU program in the GTA IV inventory
lifts.

### Phase 4: RSX to Metal (4–6 months)

- [ ] FIFO parser, state tracking, shader translation, surface and texture caches, vertex pulling, flip.
- [ ] Minimal iOS test host (no UI) so device frame times are visible from here on.
- [ ] Test with PSL1GHT graphics samples, then GTA IV's logos and menu.

**Exit:** GTA IV renders its menu correctly on macOS and on the iOS test host.

### Phase 5: GTA IV bring-up (4–8 months)

- [ ] Boot → menu → new game or save → walking → driving. Implement missing modules as logs demand.
- [ ] Audio, input, save data.
- [ ] A regression test for each fixed bug that reproduces without game data.

**Exit:** a 30-minute session on macOS covering walking, driving and one mission, with no crash and correct visuals.

### Phase 6: Performance (2–4 months)

- [ ] Profile on Mac and device: Instruments Time Profiler, Metal System Trace, thermal state.
- [ ] Patch readbacks, replace hot SPU middleware, convert spin-waits, ship the precompiled shader archive, enable
  MetalFX.

**Exit:** worst-case frame at or under 22 ms on the target iPhone during a scripted drive loop. In a 30-minute soak, no
frame over 33.3 ms after the first 5 minutes.

### Phase 7: iOS app (1–2 months)

- [ ] App shell, ISO import and verification, entitlements, controller and touch input, lifecycle, saves.

**Exit:** fresh install on device → pick ISO → play, meeting the Phase 6 targets.

## 11. Repository layout and technology

One monorepo. Generated code goes to `build/generated/<title>/` and is never committed.

```
ps3ios/
  tools/ps3inspect/        Phase 0 inventory tool
  recomp/common/           ELF/PRX loader, NID database, ISO9660 reader, manifest format
  recomp/ppu/              analyzer, lifter, codegen
  recomp/spu/              extractor, analyzer, lifter
  runtime/core/            guest memory, big-endian accessors, threads, logging
  runtime/ppu/             PPU interpreter (oracle + fallback), dispatch table
  runtime/lv2/             syscalls
  runtime/hle/             cell* modules, one file per module
  runtime/spu/             SPU interpreter, local store, MFC, channels, reservations
  runtime/spu/replace/     native replacements for known SPU programs
  runtime/rsx/             FIFO, state tracking, surface and texture caches
  runtime/rsx/metal/       Metal backend, MSL shader translator
  runtime/platform/macos/  dev host: window, input, audio
  runtime/platform/ios/    iOS host glue
  app/ios/                 SwiftUI shell, Xcode project
  games/gta4/              manifest: hashes, overrides, patches, SPU table, shader list
  tests/ppu/ tests/spu/ tests/rsx/ tests/homebrew/
  third_party/             pinned dependencies
  docs/                    this plan, decisions.md
```

### Technology choices

- C++20 for toolchain and runtime. The generated code is C, metal-cpp gives direct Metal access, and the reference
  projects (RPCS3, XenonRecomp) are C++.
- Generated code: C11 with NEON intrinsics (`<arm_neon.h>`), compiled by Apple clang.
- Swift and SwiftUI for the app shell only.
- Build: CMake for toolchain and runtime; an Xcode project for the app, consuming the runtime as a static library plus
  the generated sources.
- Tests: Catch2 or GoogleTest, plus fuzz harnesses for the differential tests.
- Dependencies: keep minimal: fmt, xxHash, zlib, metal-cpp.

## 12. Working rules for Claude Code

Copied into `CLAUDE.md` at the repo root.

## 13. Risks and mitigations

The top two risks are correctness bugs, not performance: they surface hours into play and are the hardest to trace.

| Risk | Impact | Mitigation |
|---|---|---|
| Subtle instruction-semantics bugs (carry, rounding, VMX lane order) | Wrong physics or crashes deep into a session | Interpreter oracle, per-instruction fuzzing, ps3autotests |
| SPU sync and atomics bugs | Deadlocks, lost updates, random corruption | One shared reservation system, litmus stress tests, hours-long runs |
| SPU code loaded at runtime that traces missed | Interpreter fallback, frame drops | Long trace sessions across all game areas; log-and-rebuild loop |
| Unknown indirect PPU targets | Abort or slow fallback | Aggressive pointer scanning in data, trace runs |
| Render-target readbacks on a tile-based GPU | Large frame-time spikes | Log every readback, patch per game |
| Thermal throttling | Drops below 30 fps after 15–20 minutes | 22 ms worst-case budget, native-resolution render plus MetalFX, 30-minute soak test |
| iOS memory limits | App killed under load | Entitlements, cache trimming on memory warnings, measure peak memory from Phase 4 |
| Licence conflict with GPL reference code | Late rewrite | Decide the licence before Phase 1 |
| Legal takedown | Loss of the project if public | Private repo, no game data, no distribution |
| Scope and motivation | Project stalls midway | Phase gates with visible results; homebrew milestones before GTA IV |

## 14. Open decisions

Settle the first two before Phase 1; the rest can wait for the Phase 0 inventory. Current state: `docs/decisions.md`.

- [ ] **Licence:** GPLv2, which allows reusing RPCS3 code and costs nothing for a private project, or permissive with
  clean-room code throughout. Recommendation: GPLv2.
- [ ] **Game version:** which GTA IV release and region the manifest targets, by title ID (for example BLUS30127 for US
  or BLES00229 for EU; confirm against your disc).
- [ ] **Toolchain:** reuse PS3Recomp or write our own, decided by the Phase 0 evaluation of its licence, quality and
  ARM64 output.
- [ ] **SPURS:** HLE or LLE, decided by how heavily the inventory shows GTA IV using it.
- [ ] **Target devices:** minimum iPhone chip, and whether M-series iPads are the primary target.
- [ ] **Input:** controller required, or a polished touch overlay as well.
- [ ] **ISO handling:** extract assets (simpler, doubles storage use) or read directly from the ISO.

## 15. Reference material

Read the hardware manuals for semantics and RPCS3 for how real games behave; where they disagree, real games win.

### Hardware and ISA

- IBM, *Cell Broadband Engine Programming Handbook*: PPU and SPU architecture, MFC, channels, atomics.
- IBM, *Synergistic Processor Unit Instruction Set Architecture* v1.2: every SPU instruction.
- *PowerPC Architecture Book* and the *AltiVec Technology Programming Environments Manual*: PPU integer,
  floating-point and VMX semantics.

### PS3 software

- RPCS3: lv2 syscalls, HLE modules, PPU and SPU semantics, RSX shader decompilers, guest memory map.
- ps3autotests: hardware-verified test programs.
- PS3 Developer Wiki: file formats, NIDs, syscalls.
- PSL1GHT: open-source PS3 SDK, the source of homebrew test programs.

### Static recompilation

- XenonRecomp: PowerPC and VMX128 static recompilation, the closest existing design.
- UnleashedRecomp: a complete, shipped port built with it.
- N64Recomp: where the approach started.
- PS3Recomp coverage: an existing PS3 toolkit to evaluate in Phase 0.

### Apple

- Metal Shading Language specification, MetalFX, MTLBinaryArchive, Game Controller framework, entitlements
  documentation.
