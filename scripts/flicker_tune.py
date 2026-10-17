#!/usr/bin/env python3
"""FrameWatch's flicker test over recorded runs, for its thresholds (D-1090).

    scripts/flicker_tune.py RUN... [--diff D] [--back B] [--list] [--grid] [--known FLIP...]

Each RUN is a bench run folder (or a thumbs.bin file, pspocket-boot --thumbs). Runs the test of
runtime/loader/src/frame_watch.cpp on every frame in order: frames A, B, C (consecutive presented frames) flicker when
B is far from both (mean difference over D, of 255) and C is close to A again (under B times the smaller of the two):
something drawn in one frame only. --list prints every frame that passes with its numbers; --grid counts the frames
that pass for a range of D and B in each run. --known FLIP... are frames that do flicker (a one-frame glitch seen in the
saved frames): the grid marks which settings find each of them. Unlike FrameWatch, nothing is rate-limited.
"""

import argparse
import os
import struct
import sys

W, H = 64, 36
FRAME = W * H * 4


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "thumbs.bin")
    with open(path, "rb") as f:
        data = f.read()
    frames = []
    for o in range(0, len(data) - 8 - FRAME + 1, 8 + FRAME):
        flip = struct.unpack_from("<Q", data, o)[0]
        t = data[o + 8:o + 8 + FRAME]
        frames.append((flip, bytes(t[i] for i in range(FRAME) if i % 4 != 3)))
    return frames


def difference(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def triples(frames):
    """(flip of C, ab, bc, ac) for each consecutive A, B, C."""
    out = []
    prev_ab = None
    for i in range(2, len(frames)):
        (_, a), (_, b), (fc, c) = frames[i - 2], frames[i - 1], frames[i]
        ab = prev_ab if prev_ab is not None else difference(a, b)
        bc = difference(b, c)
        prev_ab = bc
        if ab < 5 and bc < 5:
            continue # no candidate: both steps small (skips the third difference)
        out.append((fc, ab, bc, difference(a, c)))
    return out


def passes(t, d, back):
    _, ab, bc, ac = t
    return ab > d and bc > d and ac < back * min(ab, bc)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--diff", type=float, default=20.0)
    ap.add_argument("--back", type=float, default=1 / 3)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--grid", action="store_true")
    ap.add_argument("--known", type=int, nargs="*", default=[])
    args = ap.parse_args()
    data = {}
    for r in args.runs:
        frames = load(r)
        data[r] = triples(frames)
        hits = [t for t in data[r] if passes(t, args.diff, args.back)]
        print(f"{r}: {len(frames)} frames, {len(hits)} flicker at diff {args.diff} back {args.back:.2f}")
        if args.list:
            for t in hits:
                print(f"  flip {t[0]}: ab {t[1]:.1f} bc {t[2]:.1f} ac {t[3]:.1f}")
    if args.grid:
        diffs = [8, 10, 12, 15, 20, 25, 30, 40]
        backs = [0.2, 0.25, 1 / 3, 0.5]
        print("count of frames that pass, a run a column; * a known one found")
        for d in diffs:
            for b in backs:
                cells = []
                for r in args.runs:
                    hits = [t for t in data[r] if passes(t, d, b)]
                    found = {k for k in args.known for t in hits if abs(t[0] - k) <= 1}
                    cells.append(f"{len(hits):5d}{'*' * len(found):<3}")
                print(f"  diff {d:5.1f} back {b:.2f}: " + " ".join(cells))
    return 0


if __name__ == "__main__":
    sys.exit(main())
