#!/usr/bin/env python3
"""Checks one bench run (D-1060): the run's end, partly black frames in play, CPU ms a frame by workload, new message
shapes; against the case's rolling baseline.

    scripts/bench_check.py RUN_DIR --case NAME --mode MODE --from-s S [--baseline FILE] [--messages FILE]
                           [--accept | --accept-clean] [--keep N]

RUN_DIR holds run.log, summary.json and frames/ (pspocket-boot --frames, every 60th frame). Writes RUN_DIR/result.json
and prints one line. Frames become 64x36 grey thumbnails (ffmpeg); all but the N worst are deleted (game data, never
committed). --accept adds the run to the baseline (the median of its last 5 accepted runs is what the next is held to)
and its message shapes to the known ones.

Fails: the run did not reach its timeout (a fault or an exit), RSX failures, unimplemented calls, partly black frames
in play (a fifth to 95% of the thumbnail exactly black: a full black frame is a fade), new warning or error shapes. Warns:
CPU ms a frame 10% over the baseline in total or 15% for a workload, fps 10% under it.
"""
import glob
import json
import os
import re
import statistics
import subprocess
import sys
from collections import defaultdict

FPS = re.compile(r"(?:boot|game): ([0-9.]+) fps \((\d+) frames in ([0-9.]+) s; (\d+) since the boot\)")
THREADS = re.compile(r"(?:boot|game): threads: CPU ([0-9.]+) s in ([0-9.]+) s")
SPURS = re.compile(r"spurs: SPURS (0x[0-9a-f]+), SPU time in ([0-9.]+) s on \d+ SPUs: (.*)")
DRAWS = re.compile(r"rsx.metal: per frame \(last \d+\): GPU ([0-9.]+) ms, (\d+) draws")
NAMED = re.compile(r"(pspocket\.rsx|PPU 0x1000000 main_thread) ([0-9.]+) s")
SHAPE = re.compile(r"^\[(warn|error|fatal)\] (.*)")
W, H = 64, 36


def windows(log, from_s):
    """The 5 s windows after from_s seconds of the run: fps, draws, CPU ms a frame in total and by workload."""
    out, cur, draws, gpu, t = [], None, None, None, 0.0
    for line in open(log, errors="replace"):
        m = DRAWS.search(line)
        if m:
            gpu, draws = float(m.group(1)), int(m.group(2))
        m = FPS.search(line)
        if m:
            if cur and cur.get("frames"):
                out.append(cur)
            fps, frames, secs = float(m.group(1)), int(m.group(2)), float(m.group(3))
            t += secs
            cur = {"t": t, "fps": fps, "frames": frames, "secs": secs, "since": int(m.group(4)), "draws": draws,
                   "gpu_ms": gpu, "work": defaultdict(float)} if t >= from_s else None
            continue
        if cur is None:
            continue
        m = THREADS.search(line)
        if m:
            cur["cpu_s"] = float(m.group(1))
            for name, s in NAMED.findall(line):
                cur["work"]["rsx" if name == "pspocket.rsx" else "ppu_main"] += float(s)
        m = SPURS.search(line)
        if m:
            inst, rest = m.group(1), m.group(3).split("; empty")[0]
            for name, s in re.findall(r"([A-Za-z_/][A-Za-z0-9_/ ]*?) ([0-9.]+) s", rest):
                name = name.strip()
                cur["work"]["mixer" if inst == "0x10016000" else name] += float(s)
    if cur and cur.get("frames"):
        out.append(cur)
    return out


def budget(ws):
    """Medians over the windows: fps, draws, CPU ms a frame in total and for each workload."""
    if not ws:
        return {}
    def ms(w, s):
        return 1000.0 * s / max(w["frames"], 1)
    b = {"windows": len(ws), "fps": statistics.median(w["fps"] for w in ws),
         "draws": statistics.median(w["draws"] or 0 for w in ws),
         "cpu_ms": statistics.median(ms(w, w.get("cpu_s", 0.0)) for w in ws), "work_ms": {}}
    names = set()
    for w in ws:
        names |= set(w["work"])
    for n in sorted(names):
        b["work_ms"][n] = round(statistics.median(ms(w, w["work"].get(n, 0.0)) for w in ws), 2)
    return b


def frames_check(frame_dir, from_flip, keep):
    """Thumbnails of the dumped frames: the partly black ones in play, worst kept."""
    files = sorted(glob.glob(os.path.join(frame_dir, "frame-*.png")))
    if not files:
        return {"frames": 0, "partly_black": [], "kept": []}
    raw = subprocess.run(["ffmpeg", "-v", "error", "-pattern_type", "glob", "-i", os.path.join(frame_dir, "frame-*.png"),
                          "-vf", f"scale={W}:{H},format=gray", "-f", "rawvideo", "-"], capture_output=True).stdout
    n = min(len(files), len(raw) // (W * H))
    rows, bad = [], []
    for i in range(n):
        px = raw[i * W * H:(i + 1) * W * H]
        flip = int(re.search(r"frame-(\d+)", files[i]).group(1))
        # Exactly black: rendering with fog and light all but never gives 0 over an area (a night street from the air:
        # 2.5% of the thumbnail; the black sea of the research bench: 56%, whole rows).
        dark = sum(1 for p in px if p == 0) / (W * H)
        rows.append((flip, dark))
        if flip >= from_flip and 0.2 <= dark < 0.95:
            bad.append({"flip": flip, "black": round(dark, 3)})
    worst = sorted(bad, key=lambda r: -r["black"])[:keep]
    kept = {f"frame-{r['flip']:06d}.png" for r in worst}
    for f in files:
        if os.path.basename(f) not in kept:
            os.remove(f)
    return {"frames": n, "partly_black": bad, "kept": sorted(kept)}


def shapes(log):
    """Warning, error and fatal lines with numbers and addresses folded."""
    out = set()
    for line in open(log, errors="replace"):
        m = SHAPE.match(line)
        if m:
            out.add(re.sub(r"0x[0-9a-fA-F]+|\d+", "#", line.strip())[:200])
    return out


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--case", required=True)
    ap.add_argument("--mode", required=True)
    ap.add_argument("--from-s", type=float, default=0)
    ap.add_argument("--baseline", default="inventory/bench/baseline.json")
    ap.add_argument("--messages", default="inventory/bench/messages.json")
    ap.add_argument("--accept", action="store_true")
    ap.add_argument("--accept-clean", action="store_true")
    ap.add_argument("--keep", type=int, default=6)
    a = ap.parse_args(argv[1:])
    log, summ = os.path.join(a.run_dir, "run.log"), os.path.join(a.run_dir, "summary.json")
    fails, warns = [], []
    summary = json.load(open(summ)) if os.path.exists(summ) else {}
    if not summary:
        fails.append("no summary: the run died")
    elif summary.get("exit") != "timeout":
        fails.append(f"ended by {summary.get('exit')} ({summary.get('exit_status')}) before its timeout")
    for key, what in (("rsx_failures", "RSX failures"), ("unimplemented_syscalls", "unimplemented syscalls"),
                      ("unimplemented_functions", "unimplemented functions")):
        if summary.get(key):
            fails.append(f"{summary[key]} {what}")
    ws = windows(log, a.from_s)
    b = budget(ws)
    from_flip = min((w["since"] - w["frames"] for w in ws), default=0) # the first frame of the play windows
    fr = frames_check(os.path.join(a.run_dir, "frames"), from_flip, a.keep)
    if fr["partly_black"]:
        fails.append(f"{len(fr['partly_black'])} partly black frame(s) in play, the worst {fr['partly_black'][0]['black']:.0%} "
                     f"at flip {fr['partly_black'][0]['flip']}")
    known = set(json.load(open(a.messages))) if os.path.exists(a.messages) else set()
    seen = shapes(log)
    new = sorted(seen - known) if known else []
    if new:
        fails.append(f"{len(new)} new warning/error shape(s): {new[0][:120]}")
    key = f"{a.case}/{a.mode}"
    base = json.load(open(a.baseline)) if os.path.exists(a.baseline) else {}
    hist = base.get(key, [])
    if hist and b:
        ref_cpu = statistics.median(h["cpu_ms"] for h in hist)
        ref_fps = statistics.median(h["fps"] for h in hist)
        if b["cpu_ms"] > 1.10 * ref_cpu:
            warns.append(f"CPU {b['cpu_ms']:.1f} ms a frame, baseline {ref_cpu:.1f} (+{b['cpu_ms'] / ref_cpu - 1:.0%})")
        if b["fps"] < 0.90 * ref_fps:
            warns.append(f"fps {b['fps']:.1f}, baseline {ref_fps:.1f}")
        for n, v in b["work_ms"].items():
            refs = [h["work_ms"].get(n) for h in hist if h["work_ms"].get(n)]
            if refs and v > 1.15 * statistics.median(refs) and v > 1.0:
                warns.append(f"{n} {v:.1f} ms a frame, baseline {statistics.median(refs):.1f}")
    result = {"case": a.case, "mode": a.mode, "verdict": "FAIL" if fails else ("WARN" if warns else "PASS"),
              "fails": fails, "warns": warns, "budget": b, "frames": fr, "summary": summary,
              "baseline_runs": len(hist)}
    json.dump(result, open(os.path.join(a.run_dir, "result.json"), "w"), indent=1)
    # --accept-clean (the nightly bench, D-1067): only a run with no warning joins, until the baseline has 3 runs.
    if b and not fails and (a.accept or (a.accept_clean and (not warns or len(hist) < 3))):
        base[key] = (hist + [{"cpu_ms": b["cpu_ms"], "fps": b["fps"], "work_ms": b["work_ms"]}])[-5:]
        os.makedirs(os.path.dirname(a.baseline) or ".", exist_ok=True)
        json.dump(base, open(a.baseline, "w"), indent=1)
        json.dump(sorted(known | seen), open(a.messages, "w"), indent=1)
    print(f"{result['verdict']:4}  {key:24} fps {b.get('fps', 0):5.1f}  CPU {b.get('cpu_ms', 0):6.1f} ms/frame  "
          f"draws {b.get('draws', 0):5.0f}  {'; '.join(fails + warns) or 'ok'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
