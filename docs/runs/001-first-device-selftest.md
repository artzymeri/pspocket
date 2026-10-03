# Run 001 · First device self-test

2026-10-03 · build e557a8a (Release) · iPhone 17 Pro Max (iPhone18,2, A19 Pro), iOS 26.6.1 (23G83) · no debugger ·
signed by team M7XJA39ZV6 with both kernel memory entitlements (`codesign -d --entitlements -` checked).

Commands: `scripts/ios.sh build`, `install`, `run --autorun --exit`, `report`; then `run -- -tab metal -passes 32`.

## Results

| Probe | iPhone 17 Pro Max | M4 Mac (same build) | Meaning for the plan |
|---|---|---|---|
| `mem.reserve` | 4 GB + guard reserved in 0.009 ms; 527 MB committed; first touch 0.69–1.08 µs per 16 KB page | 2.4–2.7 µs/page | The extended-virtual-addressing entitlement works; the 4 GB guest model (section 4) holds on iOS |
| available memory | 5.98 GB for the process (11.4 GB physical) | — | increased-memory-limit is in effect; budget for guest 512 MB + RSX copies + caches is comfortable |
| `mem.bandwidth` (volatile BE scalar, D-013) | read 14.8 GB/s, write 14.0 GB/s, memcpy 29.3 GB/s | 12.2 / 10.6 / 20.0 GB/s | Guest accessors cost one load + REV; the phone is faster than the Mac here |
| `mem.fault_tracking` | 256 faults, 3.67 µs round trip each | 3.68 µs | Fault-based write tracking works in-process on iOS without a debugger: the hash-on-bind fallback (section 4) isn't needed. 3.7 µs × a few hundred texture pages per frame is affordable; thousands are not |
| `timebase` | 79.800 MHz | 79.800 MHz | `mftb` scaling is exact |
| `threads.qos` | 73–82 ms for every class | 68–84 ms | An idle phone runs even background QoS on a performance core; P/E behaviour needs a loaded run to show |

Host facts: 6 cores (2 performance, 4 efficiency), 16 KB pages.

## Output preview (Metal tab, 32+ synthetic passes)

```
1152×640 → 1320×733 MetalFX spatial | cpu 1.2–2.1 ms | gpu 5.0–7.7 ms (max 22 ms) | frame 33.3 ms | dropped 0/304 | thermal nominal | 105 MB
```

- MetalFX spatial upscaling works on the A19 Pro (the simulator lacks it). Portrait aspect-fit target 1320×733.
- 30 Hz pacing holds: average frame 33.3 ms, 0 dropped over 10 s; the worst 5-second interval contained one 41.7 ms
  presentation (a single 120 Hz-tick late present).
- The load pass count went from 32 to 59 at 10 s (slider moved on the phone); output stopped after 10 s when the app
  left the Metal tab or the foreground. A sustained thermal soak (plan section 2: 22 ms budget, 30-minute run) is still
  to do: `scripts/ios.sh run -- -tab metal -passes 48`, phone untouched, for 30 minutes.

## Raw report

```
build:   pspocket 0.1.0 (e557a8a695), release, clang 17.0.0 (clang-1700.3.19.1), iOS arm64
device:  iPhone18,2, iOS 26.6.1 (23G83)
start:   thermal nominal, low power mode off, footprint 20 MB, available 5.98 GB

== platform           PASS  Darwin 25.6.0              cores 6 (2P+4E), 11.4 GB, 16 KB pages
== mem.reserve        PASS  4 GB reserved, 527 MB committed and verified  [44 ms]
== mem.bandwidth      PASS  read 14.8 GB/s, write 14.0 GB/s  [51 ms]
== mem.fault_tracking PASS  256 faults handled, 3.67 us each  [1 ms]
== timebase           PASS  79.80 MHz  [105 ms]
== threads.qos        PASS  interactive 82 ms, background 73 ms  [301 ms]

6 passed, 0 failed, 0 skipped (0.50 s)
end:     thermal nominal, low power mode off, footprint 87 MB
```
