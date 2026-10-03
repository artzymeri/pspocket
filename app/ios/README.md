# PSPocket iOS test host

A small SwiftUI app that checks the platform assumptions the runtime depends on, on the real phone (D-008):
signing with the two memory entitlements, the 4 GB guest reservation, fault-based write tracking, timer and core
speeds, and the planned output path (1152×640 upscaled with MetalFX, paced at 30 Hz). No game code or data.

- **Self-test** tab: runs the probes from `runtime/selftest` (the same ones `pspocket-selftest` runs on the Mac) and
  writes a plain-text report.
- **Metal** tab: a synthetic GPU load rendered at 1152×640, upscaled to the screen and paced at 30 Hz, with frame-time,
  GPU-time, dropped-frame, thermal and memory numbers on screen.

The runtime is compiled by Xcode from the same `runtime/` sources the CMake host build uses (D-007): `project.yml`
defines a static library (`PSPocketRuntime`) and the app links it.

## One-time setup

- **Xcode signed in to the team.** Xcode > Settings > Accounts must list the Apple ID of team M7XJA39ZV6. The first
  device build then registers `com.artzymeri.pspocket.testhost` with the Extended Virtual Addressing and Increased
  Memory Limit capabilities and creates its provisioning profile (`-allowProvisioningUpdates`). Without an account the
  build stops with "No Accounts".
- **iPhone:** Developer Mode on, connected by cable, this Mac trusted, unlocked while launching.
- XcodeGen (`brew install xcodegen`), CMake and Ninja.

## Commands

`scripts/ios.sh` works from any directory. Run `scripts/ios.sh --help` for the full list.

| Command | What it does |
|---|---|
| `scripts/ios.sh all` | Build, install, run every probe, quit, and fetch the report. **The usual one.** |
| `scripts/ios.sh build [--sim] [--debug]` | Builds the host CMake tree (it generates the lifted self-test code), runs `xcodegen`, then `xcodebuild`. Release by default. Prints the `.app` path. |
| `scripts/ios.sh install [--device ID]` | Installs the last build with `devicectl` (first connected iPhone by default). |
| `scripts/ios.sh run [--autorun] [--exit] [-- ARGS]` | Launches with the console attached. Output also goes to `build/ios/last-run.log`. Ctrl-C stops the app. |
| `scripts/ios.sh report` | Copies `Documents/selftest-report.txt` from the app to `build/ios/selftest-report.txt` and prints it. |
| `scripts/ios.sh gen` | Only the host build and `xcodegen` (`app/ios/PSPocket.xcodeproj`, not committed). |

Add `--sim` to `install`, `run`, `report` or `all` to use an iPhone simulator instead (`PSPOCKET_SIMULATOR` picks
one by name or UDID). The simulator has no MetalFX, no presentation timestamps and no real memory limits, so its
numbers mean nothing; it is for checking that the app works.

Environment: `PSPOCKET_HOST_BUILD` (host CMake build, default `build/host`), `PSPOCKET_IOS_BUILD` (Xcode output,
logs, report; default `build/ios`), `PSPOCKET_DEVICE` (default for `--device`).

If the host build fails, `gen` warns and carries on with whatever generated sources exist: Xcode compiles the runtime
sources itself. If the phone is locked, `run` stops with "Unlock the iPhone and run again".

### Launch options

Passed by `run --autorun --exit`, or after `--` (`scripts/ios.sh run -- -tab metal -passes 32`). Each one also works as
an environment variable.

| Argument | Environment | Effect |
|---|---|---|
| `-autorun` | `PSPOCKET_AUTORUN=1` | Run every probe at launch. |
| `-autorun-exit` | `PSPOCKET_AUTORUN_EXIT=1` | Same, then quit once the report is written (exit code 1 if a probe failed). |
| `-tab metal` | `PSPOCKET_TAB=metal` | Start on the Metal tab, so the load runs immediately. |
| `-passes N` | `PSPOCKET_PASSES=N` | Metal load passes at launch (0–64, default 8). |

Every self-test line goes to stdout, so `run` shows it live. On the Metal tab the app prints a stats line every 5 s.

## Self-test probes

| Probe | Checks |
|---|---|
| `platform` | Kernel, model, CPU, core counts (performance and efficiency), RAM, `process_available_memory` (what jetsam allows; the increased-memory-limit entitlement raises it), host page size, debugger attached. |
| `mem.reserve` | Reserves the 4 GB guest space plus guard (needs the extended-virtual-addressing entitlement), commits main memory, RSX local memory and a stack region, touches and verifies every page, checks the last guest page, decommits. Reports commit and first-touch times. |
| `mem.bandwidth` | Big-endian 32-bit loads and stores through guest memory, and `memcpy`, in GB/s. |
| `mem.fault_tracking` | Write tracking by page protection: write-protects 256 pages, handles one SIGSEGV/SIGBUS per page (mark dirty, unprotect, resume), reports the cost per fault. Skipped while a debugger is attached. |
| `timebase` | The guest timebase runs at 79.8 MHz (within 2%). |
| `threads.qos` | The same integer loop on a thread of each QoS class: shows which classes get performance cores. |

Later phases add probes (for example PPU interpreter and lifted-code speed); the app lists whatever
`ps_selftest_probe_count()` returns, so new probes appear without app changes. Long-press or swipe a row to run one
probe alone.

The report (`Documents/selftest-report.txt`, also visible in Files > On My iPhone > PSPocket, and behind the
Copy report and Share buttons) has the build, device and iOS version, Low Power Mode, thermal state and memory
footprint at start and end, every probe's lines, runtime log lines (`[warn] mem: …`), and per-probe durations.

## Metal tab

The scene is one base pass plus N load passes (slider, 0–64). Each pass is its own render pass over the 1152×640
target (load, draw a procedural pattern, store), so the cost includes the render-target traffic a game causes on a
tile-based GPU. The result is upscaled to the screen with the MetalFX spatial scaler (bilinear if MetalFX is
unavailable) and letterboxed, and frames are presented on a 30 Hz `CADisplayLink` grid. A white bar sweeps across at
constant speed, which makes uneven pacing easy to see.

| Overlay line | Meaning |
|---|---|
| `1152×640 → W×H …` | Output size on this screen and the scaling path. |
| `CPU` | Time to encode a frame on the main thread (average over 1 s, maximum over 5 s). |
| `GPU` | `gpuEndTime − gpuStartTime` of the frame's command buffer (average 1 s, maximum 5 s). |
| `frame` | Interval between frames on screen (`presentedTime`; display-link timestamps in the simulator), average and worst over 5 s. 33.33 ms is on target. |
| `dropped` | Frames that missed their 30 Hz slot, counted from the intervals, since start or Reset stats. |
| `thermal`, `footprint` | `ProcessInfo.thermalState`, and the process memory footprint (`phys_footprint`, what jetsam counts). |

Rendering stops whenever the Metal tab is off screen or the app is in the background, so it does not load the GPU
behind the Self-test tab. The screen does not auto-lock while the app is open.

## What to report back

1. The whole report: `build/ios/selftest-report.txt` after `scripts/ios.sh all` (or Copy report on the phone).
2. Metal tab, phone at room temperature and not charging: after 30 s at **0, 16, 32 and 64** passes, the CPU, GPU,
   frame/worst and dropped lines.
3. A thermal run: `scripts/ios.sh run -- -tab metal -passes 32` for 10 minutes (Ctrl-C to stop), then
   `build/ios/last-run.log`. It has the GPU time, worst frame, dropped frames and thermal state every 5 s.
4. Anything that failed or looked wrong, with the exact message.

## Notes

- The scheme builds and runs Release, also from Xcode: the probe numbers only mean something optimised. Build with
  `--debug` (or switch the scheme) to debug. With a debugger attached, `mem.fault_tracking` is skipped; in LLDB,
  `process handle -p true -s false SIGSEGV SIGBUS` lets the intentional faults through (plan section 4).
- The app shows the build info from `ps_selftest_build_info()`: version and commit come from `scripts/ios.sh`
  (`-dirty` when tracked files have uncommitted changes); a build started from Xcode shows `unknown`.
- Lifted self-test code is generated by the host build into `<host build>/generated/selftest/` and compiled into the
  runtime library when it exists; `build` regenerates the Xcode project each time, so new files are picked up.
