# pspocket

A per-game static recompiler that turns a PS3 game's PPU and SPU code into a native, signed iOS app. Game code is
translated to C on the Mac ahead of time and compiled into the app; at runtime the app only reads assets from the
user's own disc dump. No JIT, no interpreter on the hot path. First title: GTA V (BLES01807, D-025).

- Plan: [docs/plan.md](docs/plan.md) · Decisions: [docs/decisions.md](docs/decisions.md) · Rules: [CLAUDE.md](CLAUDE.md)
- Private, personal project. The repo contains no Sony or Rockstar code, keys, firmware or assets, and never will.

## Status

| Phase | Gate | State |
|---|---|---|
| 0 · Inventory and go/no-go | Inventory report, go/no-go | GTA V inventory done, **go** ([docs/phase0-gta5.md](docs/phase0-gta5.md)); runtime trace (job chains) still to do |
| 1 · Toolchain foundation | Samples recompile and match the interpreter; GTA V ELF lifts | Loader, NID database, manifest; PPU interpreter and lifter including VMX (D-028), differential fuzz green. Re-lifting the GTA V ELF with VMX: to do on the Mac |
| 2 · Runtime core on macOS | Test suite passes; EBOOT boots to its first unimplemented call | Guest memory, timebase, shared reservations (D-030), lv2 kernel: threads, sync, events, memory, time, fs (D-031). Next: firmware-module (HLE) layer and the boot host |
| 3 · SPU | Stress runs, every inventoried SPU program lifts | SPU interpreter, channels, MFC and lifter (D-029); SPURS not started (needs the firmware file, D-024) |
| 4 · RSX to Metal | Menu renders on macOS and iOS | FIFO, methods, state, shader decode to MSL, textures, write tracking, Metal backend (D-032); iOS test host proven on device ([runs](docs/runs/README.md)) |
| 5–7 · Bring-up, performance, app | | Not started |

## Build

```sh
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON
ninja -C build/host
ctest --test-dir build/host --output-on-failure
./build/host/runtime/platform/macos/pspocket-selftest
```

Clang only (D-026). On Linux: `CC=clang CXX=clang++ cmake ...`; the AArch64-only paths are tested under qemu with
`-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64-qemu.cmake` (D-027).

iOS test host on a connected iPhone: `scripts/ios.sh all` (see [app/ios/README.md](app/ios/README.md)).

## Layout

See [CLAUDE.md](CLAUDE.md#layout).
