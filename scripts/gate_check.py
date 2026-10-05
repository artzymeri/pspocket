#!/usr/bin/env python3
"""Checks a pspocket-boot --summary against a game's gate thresholds (games/<title>/gate.json, D-149).

    gate_check.py SUMMARY.json GATE.json [--messages BASELINE.json] [--accept-messages]

Prints one line per check (PASS or FAIL, the value and the limit) and exits 1 if any check failed. Warning and error
messages are compared by shape with BASELINE.json (kept in inventory/gate/: the messages name the game's files, so the
list stays out of git, CLAUDE.md rule 1); a shape not in it fails the gate, and --accept-messages adds the run's shapes
to it instead.
"""

import json
import statistics
import sys


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 64
    summary = json.load(open(argv[1]))
    gate = json.load(open(argv[2]))
    baseline_path = None
    accept = "--accept-messages" in argv
    if "--messages" in argv:
        baseline_path = argv[argv.index("--messages") + 1]
    c = gate["checks"]
    rows = []

    def check(name, value, limit, ok):
        rows.append((name, value, limit, ok))

    check("run ends at its timeout (no fault, no exit)", summary["exit"], c["exit"], summary["exit"] == c["exit"])
    check("RSX failures", summary["rsx_failures"], f"<= {c['rsx_failures_max']}",
          summary["rsx_failures"] <= c["rsx_failures_max"])
    check("unimplemented syscalls", summary["unimplemented_syscalls"], "0", summary["unimplemented_syscalls"] == 0)
    check("unimplemented functions", summary["unimplemented_functions"], "0", summary["unimplemented_functions"] == 0)
    spu = summary["spu_loads"]
    check("SPU loads interpreted", spu["interpreted"], "0", spu["interpreted"] == 0)

    fps = summary["fps"][c["fps_skip_windows"]:]
    if fps:
        median = statistics.median(fps)
        check("fps, median of the 5 s windows", f"{median:.1f}", f">= {c['fps_median_min']}", median >= c["fps_median_min"])
        check("fps, slowest 5 s window", f"{min(fps):.1f}", f">= {c['fps_floor']}", min(fps) >= c["fps_floor"])
    else:
        check("fps windows", 0, "some", False)

    jumps = summary["frame_jumps"]
    check("frames compared", jumps["compared"], f">= {c['frames_compared_min']}",
          jumps["compared"] >= c["frames_compared_min"])
    check("frame jumps (64x36 mean difference > 25)", jumps["jumps"], f"<= {c['frame_jumps_max']}",
          jumps["jumps"] <= c["frame_jumps_max"])

    windows = summary["gpu_windows"]
    play = [w for w in windows if w["last_flip"] > c["play_from_flip"]]
    if play:
        gpu = max(w["gpu_ms"] for w in play)
        check("GPU ms per frame, worst window in play", f"{gpu:.2f}", f"<= {c['gpu_ms_max']}", gpu <= c["gpu_ms_max"])
        passes = max(w["render_passes"] for w in play)
        check("render passes per frame, worst window in play", f"{passes:.1f}", f"<= {c['render_passes_max']}",
              passes <= c["render_passes_max"])
        faults = max(w["write_faults"] for w in play)
        check("write faults per frame, worst window", f"{faults:.1f}", f"<= {c['write_faults_max']}",
              faults <= c["write_faults_max"])
        made = sum(w["programs_made"] + w["pipelines_made"] for w in play)
        check("programs and pipelines compiled in play", made, f"<= {c['compiled_in_play_max']}",
              made <= c["compiled_in_play_max"])
        copies = max(w["feedback_copies"] for w in play)
        check("feedback copies per frame, worst window", f"{copies:.2f}", f"<= {c['feedback_copies_max']}",
              copies <= c["feedback_copies_max"])
        wait = max(w["readback_wait_ms"] for w in play)
        check("ms waiting for readbacks per frame, worst window", f"{wait:.2f}", f"<= {c['readback_wait_ms_max']}",
              wait <= c["readback_wait_ms_max"])
    else:
        check("GPU windows in play", 0, "some", False)
    if windows:
        gpu_mem = max(w["gpu_allocated_mb"] for w in windows)
        check("GPU memory, peak (MB)", f"{gpu_mem:.0f}", f"<= {c['gpu_allocated_mb_max']}", gpu_mem <= c["gpu_allocated_mb_max"])
    footprint = summary["footprint_peak_mb"]
    check("process memory, peak (MB)", f"{footprint:.0f}", f"<= {c['footprint_mb_max']}", footprint <= c["footprint_mb_max"])

    new_shapes = []
    if baseline_path:
        try:
            known = set(json.load(open(baseline_path)))
        except (OSError, ValueError):
            known = set()
        new_shapes = sorted(s for s in summary["messages"] if s not in known)
        if accept:
            json.dump(sorted(known | set(summary["messages"])), open(baseline_path, "w"), indent=1)
            check(f"warning and error shapes ({len(new_shapes)} new, accepted)", len(new_shapes), "-", True)
        else:
            check("warning and error shapes not seen before", len(new_shapes), "0", not new_shapes)

    width = max(len(r[0]) for r in rows)
    failed = 0
    for name, value, limit, ok in rows:
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:<{width}}  {value!s:>10}  ({limit})")
    for s in new_shapes[:20]:
        print(f"      new: {s}")
    print(f"gate: {'PASS' if not failed else f'FAIL ({failed} of {len(rows)})'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
