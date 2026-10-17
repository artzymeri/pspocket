#!/usr/bin/env python3
"""Two runs of a bench case, frame by frame at the same flips (D-1090).

    scripts/bench_compare.py RUN_A RUN_B [--over D] [--show N] [--step N] [--scene] [--slack N]

RUN_A and RUN_B are bench run folders (or thumbs.bin files) from pspocket-boot --thumbs: each frame's 64x36 BGRA
thumbnail with its flip. For each flip both runs presented, the mean absolute difference of their thumbnails (0-255,
over the three colour channels; with --scene, of their 8x4 block means: the same place, whoever stands where) is
taken; prints how many flips match, the median and 90th percentile difference,
and the stretches where the two parted by more than D (default 12): a case on the flip clock (scripts/bench.py's
default) should reach the same scenes, so its stretches are short (weather, crowds, particles) and nothing is a
different place. Exit 0 when at most a tenth of the common flips are over D, 1 otherwise.
"""

import argparse
import os
import struct
import sys

FRAME = 64 * 36 * 4


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "thumbs.bin")
    out = {}
    with open(path, "rb") as f:
        data = f.read()
    for o in range(0, len(data) - 8 - FRAME + 1, 8 + FRAME):
        flip = struct.unpack_from("<Q", data, o)[0]
        out[flip] = data[o + 8:o + 8 + FRAME]
    return out


def difference(a, b):
    total = 0
    for i in range(0, FRAME, 4):
        total += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
    return total / (64 * 36 * 3)


def coarse(t):
    """The thumbnail as 8x4 blocks of 8x9 pixels, each its mean colour: the scene, not where a character stands."""
    out = []
    for by in range(4):
        for bx in range(8):
            s = [0, 0, 0]
            for y in range(by * 9, by * 9 + 9):
                for x in range(bx * 8, bx * 8 + 8):
                    i = (y * 64 + x) * 4
                    s[0] += t[i]
                    s[1] += t[i + 1]
                    s[2] += t[i + 2]
            out.extend(v / 72 for v in s)
    return out


def scene_difference(a, b):
    ca, cb = coarse(a), coarse(b)
    return sum(abs(x - y) for x, y in zip(ca, cb)) / len(ca)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--over", type=float, default=12.0)
    ap.add_argument("--show", type=int, default=12)
    ap.add_argument("--step", type=int, default=1, help="compare every Nth common flip (faster)")
    ap.add_argument("--slack", type=int, default=0,
                    help="compare each of A's flips with the closest of B's within this many flips (a scene a little late)")
    ap.add_argument("--scene", action="store_true",
                    help="compare 8x4 block means (the scene) instead of the 64x36 thumbnails (the picture)")
    args = ap.parse_args()
    a, b = load(args.a), load(args.b)
    common = sorted(set(a) & set(b))[::args.step]
    if not common:
        print("no common flips")
        return 1
    diff = scene_difference if args.scene else difference
    if args.slack:
        bflips = sorted(b)
        import bisect
        def best(f):
            lo = bisect.bisect_left(bflips, f - args.slack)
            hi = bisect.bisect_right(bflips, f + args.slack)
            return min(diff(a[f], b[g]) for g in bflips[lo:hi:max(1, args.slack // 8)] + [f] if g in b)
        diffs = [(f, best(f)) for f in common]
    else:
        diffs = [(f, diff(a[f], b[f])) for f in common]
    values = sorted(d for _, d in diffs)
    over = [(f, d) for f, d in diffs if d > args.over]
    print(f"{len(common)} common flips ({min(common)}-{max(common)}); A {len(a)} frames, B {len(b)}; "
          f"difference median {values[len(values) // 2]:.2f}, 90th percentile {values[int(len(values) * 0.9)]:.2f}, "
          f"max {values[-1]:.2f}; {len(over)} over {args.over}")
    stretches = []
    for f, d in over:
        if stretches and f - stretches[-1][1] <= 60 * args.step:
            stretches[-1][1] = f
            stretches[-1][2] = max(stretches[-1][2], d)
        else:
            stretches.append([f, f, d])
    for s, e, m in sorted(stretches, key=lambda x: -(x[1] - x[0]))[:args.show]:
        print(f"  flips {s}-{e} ({e - s + 1}): up to {m:.1f}")
    return 0 if len(over) <= len(common) // 10 else 1


if __name__ == "__main__":
    sys.exit(main())
