# Run 003 · PPU interpreter vs lifted code on the A19 Pro

2026-10-03 · build 2bef847 (Release, built from a clean checkout) · iPhone 17 Pro Max (A19 Pro), iOS 26.6.1 ·
`scripts/ios.sh run --autorun --exit` · 8/8 probes pass.

## The new probes

The same synthetic PPU benchmark (a checksum loop over a buffer plus recursive fib: integer ops, loads/stores, calls,
branches; 75,857,305 guest instructions) runs on the interpreter and as code lifted by `ppu-lift` at build time and
compiled into the app. Results and the whole context must match.

| | iPhone 17 Pro Max | M4 Mac (same commit) |
|---|---|---|
| Interpreter | 317 MIPS (239 ms) | 233 MIPS (325 ms) |
| Lifted | 20.55 ms, ≈ 3,691 MIPS equivalent | 21.86 ms, ≈ 3,470 MIPS equivalent |
| Speedup | 12.0× | 14.1× |
| Context and buffer after the run | identical to the interpreter | identical |

## What it means

- Static recompilation does what the plan needs on the phone: one A19 Pro core runs lifted PPU code at a rate
  comparable to, or above, the real PPU on this kind of code (3.2 GHz, in-order, rarely sustaining one instruction
  per cycle on game code). This is a synthetic integer benchmark; FP and VMX-heavy code is measured once VMX lands.
- The interpreter (the release-build fallback for code the build missed) is about 12× slower, which is why the plan
  keeps it off the hot path and feeds every miss back into the next build.

## The rest of the run

Same as runs 001/002: 4 GB reserved and 527 MB committed (0.6–0.7 µs per page first touch), BE guest access
14.9/14.3 GB/s, fault round trip 2.53 µs, timebase 79.800 MHz, thermal nominal.
