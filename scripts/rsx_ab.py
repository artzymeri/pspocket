#!/usr/bin/env python3
"""Compares two pspocket-boot --summary files' draw signatures (D-1029): the draws drawn by fragment program, per frame.

    rsx_ab.py FAST.json EXACT.json [--min 0.5] [--top 30]

Meant for a run in the usual (fast) mode against one in the exact mode (PSPOCKET_RSX_EXACT=1) over the same route: a
fragment program the exact run draws and the fast one does not (or far less a frame) is a draw the fast paths lose
(a query read early, a conditional draw decided on a stale count, a draw skipped while its program compiled). The runs
are not frame-exact, so the per-frame rates are compared, with a relative threshold; the alarms (D-1022) of both runs
are printed beside. Exits 1 when some program differs by more than the threshold.
"""

import json
import sys


def load(path):
    s = json.load(open(path))
    sig = s.get("draw_signature")
    if not sig or not sig.get("flips"):
        sys.exit(f"rsx_ab: {path} has no draw signature (a run with --summary and the Metal backend)")
    return s, sig["flips"], {k: v / sig["flips"] for k, v in sig["draws"].items()}


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        print(__doc__)
        return 64
    threshold = float(argv[argv.index("--min") + 1]) if "--min" in argv else 0.5
    top = int(argv[argv.index("--top") + 1]) if "--top" in argv else 30
    (sa, fa, a), (sb, fb, b) = load(args[0]), load(args[1])
    print(f"A {args[0]}: {fa} flips, {sum(a.values()):.0f} draws a frame, {len(a)} programs")
    print(f"B {args[1]}: {fb} flips, {sum(b.values()):.0f} draws a frame, {len(b)} programs")
    for name, s in (("A", sa), ("B", sb)):
        alarms = s.get("alarms", {})
        if alarms:
            print(f"{name} alarms: " + ", ".join(f"{k} {v}" for k, v in alarms.items()))
    rows = []
    for k in set(a) | set(b):
        ra, rb = a.get(k, 0.0), b.get(k, 0.0)
        big = max(ra, rb)
        if big < 0.05:  # drawn less than once in 20 frames in both
            continue
        rel = abs(ra - rb) / big
        rows.append((rel, k, ra, rb))
    rows.sort(reverse=True)
    differ = [r for r in rows if r[0] > threshold]
    print(f"{len(differ)} of {len(rows)} programs differ by more than {threshold:.0%} a frame:")
    for rel, k, ra, rb in rows[:top]:
        mark = "*" if rel > threshold else " "
        print(f" {mark} {k}  A {ra:8.2f}  B {rb:8.2f}  ({rel:.0%})")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
