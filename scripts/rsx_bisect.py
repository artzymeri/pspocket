#!/usr/bin/env python3
"""Finds where two backend modes part on an RSX capture, and which fast path does it (D-1053).

    scripts/rsx_bisect.py CAPTURE [--a ENV=V ...] [--b ENV=V ...] [--switch ENV=V ...] [--runs N] [--keep DIR]

Replays CAPTURE (pspocket-boot --rsx-capture) through build/host/runtime/rsx/rsx-replay in mode A (default: as the
environment is, the fast paths on) and mode B (default: PSPOCKET_RSX_EXACT=1), each with --draws: a hash of every
colour target after each draw and clear. Then:
  1. the frames' hashes of each, and the first draw (or clear) whose targets differ between A and B, described
     (its targets, programs, depth, blend, textures: rsx-replay --describe);
  2. A repeated (--runs, 2): whether A gives the same draws every run (a fast path that races shows here first);
  3. each switch (the backend's fast paths, below, or --switch) added to A: whether A then matches B from that
     first draw on (the switch "explains" the difference), or its frames.
Exit 0 when A and B agree; 1 when they differ (the report says where and which switch matches, if one does).
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY = os.path.join(ROOT, "build", "host", "runtime", "rsx", "rsx-replay")

# The Metal backend's fast paths and the switches that turn them off (metal_backend.mm, surface_cache.mm).
SWITCHES = [
    ("PSPOCKET_RSX_NO_CARRY=1", "targets carried under a new one (D-1025)"),
    ("PSPOCKET_RSX_NO_SURFACE_POOL=1", "removed targets' textures reused (D-984)"),
    ("PSPOCKET_RSX_NO_PASS_MERGING=1", "render passes merged, clears folded into them (D-945)"),
    ("PSPOCKET_RSX_WRITE_BACK_TARGETS=1", "textures over a target decoded on the GPU instead of a write-back (D-137)"),
    ("PSPOCKET_RSX_STRICT_FEEDBACK=1", "a target sampled while drawn to, without a copy (D-947)"),
    ("PSPOCKET_RSX_INTERPRETER=1", "the translated shaders (the interpreter instead, D-940)"),
    ("PSPOCKET_RSX_NO_DEPTH_CLAMP=1", "depth clamping"),
    ("PSPOCKET_RSX_EARLY_REPORTS=1", "reports written whole after the GPU (D-1021): the early ones instead"),
    ("PSPOCKET_RSX_UNINIT_TARGETS=1", "uninitialised targets as memory holds them (D-1023)"),
    ("PSPOCKET_RSX_NO_GPU_PREDICATE=1", "conditional rendering decided on the GPU (D-1058): on the CPU instead"),
]


def env_of(pairs):
    env = dict(os.environ)
    for p in pairs:
        k, _, v = p.partition("=")
        env[k] = v
    return env


def replay(capture, pairs, work, tag, describe=None):
    draws = os.path.join(work, f"draws-{tag}.txt")
    cmd = [REPLAY, capture, "--quiet", "--draws", draws]
    if describe:
        cmd += ["--describe", str(describe)]
    r = subprocess.run(cmd, env=env_of(pairs), capture_output=True, text=True)
    if r.returncode == 2:
        sys.exit(f"rsx_bisect: rsx-replay failed ({' '.join(pairs) or 'default'}): {r.stderr.strip()}")
    frames = [l for l in r.stdout.splitlines() if l.startswith("flip ")]
    with open(draws) as f:
        lines = [l.split() for l in f]
    return {"frames": [l.split()[-1] for l in frames], "draws": lines, "stdout": r.stdout, "rc": r.returncode}


def first_difference(a, b):
    """The index into the draw lines where a and b part (a missing line counts), or None."""
    for i in range(max(len(a), len(b))):
        if i >= len(a) or i >= len(b):
            return i
        if a[i] != b[i]:
            return i
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--a", action="append", default=[], help="mode A's environment (ENV=V, repeatable)")
    ap.add_argument("--b", action="append", default=None, help="mode B's environment (default PSPOCKET_RSX_EXACT=1)")
    ap.add_argument("--switch", action="append", default=None, help="switches to try (default: the backend's)")
    ap.add_argument("--runs", type=int, default=2, help="A's runs, for its own determinism (2)")
    ap.add_argument("--keep", help="keep the draws files in this folder")
    args = ap.parse_args()
    if not os.path.exists(REPLAY):
        sys.exit(f"rsx_bisect: no {REPLAY} (ninja -C build/host rsx-replay)")
    b_env = args.b if args.b is not None else ["PSPOCKET_RSX_EXACT=1"]
    switches = [(s, "") for s in args.switch] if args.switch else SWITCHES
    work = args.keep or tempfile.mkdtemp(prefix="rsx-bisect-")
    os.makedirs(work, exist_ok=True)

    a = replay(args.capture, args.a, work, "a")
    b = replay(args.capture, b_env, work, "b")
    print(f"A ({' '.join(args.a) or 'as the environment is'}): frames {' '.join(a['frames'])}, {len(a['draws'])} target hashes")
    print(f"B ({' '.join(b_env)}): frames {' '.join(b['frames'])}, {len(b['draws'])} target hashes")
    for k in range(1, args.runs):
        again = replay(args.capture, args.a, work, f"a{k}")
        d = first_difference(a["draws"], again["draws"])
        if d is None:
            print(f"A run {k + 1}: the same draws")
        else:
            print(f"A run {k + 1}: parts from A's first run at line {d} (draw {a['draws'][d][0] if d < len(a['draws']) else '?'}):"
                  " A itself is not deterministic there")
    d = first_difference(a["draws"], b["draws"])
    if d is None:
        print("A and B agree on every draw" + ("" if a["frames"] == b["frames"] else ", not on the frames presented"))
        return 0 if a["frames"] == b["frames"] else 1
    line_a = a["draws"][d] if d < len(a["draws"]) else None
    line_b = b["draws"][d] if d < len(b["draws"]) else None
    n = int((line_a or line_b)[0])
    print(f"first difference: {(line_a or line_b)[1]} {n}, target {(line_a or line_b)[2]} {(line_a or line_b)[3]}: "
          f"A {line_a[4] if line_a else '-'}, B {line_b[4] if line_b else '-'}")
    desc = replay(args.capture, args.a, work, "describe", describe=n)
    for l in desc["stdout"].splitlines():
        if not l.startswith(("flip ", "replayed ")):
            print("  " + l)
    explained = []
    for sw, what in switches:
        s = replay(args.capture, args.a + [sw], work, "sw")
        ds = first_difference(s["draws"], b["draws"])
        if ds is None and s["frames"] == b["frames"]:
            verdict = "A matches B everywhere"
            explained.append(sw)
        elif ds is None or ds > d:
            verdict = f"A matches B past the first difference (to line {ds if ds is not None else 'the end'})"
            explained.append(sw)
        else:
            verdict = "no change at the first difference"
        print(f"  {sw:<36} {verdict}{'  (' + what + ')' if what else ''}")
    print("the fast path that makes it differ: " + (", ".join(explained) if explained else "none of the switches"))
    if not args.keep:
        for f in os.listdir(work):
            os.remove(os.path.join(work, f))
        os.rmdir(work)
    return 1


if __name__ == "__main__":
    sys.exit(main())
