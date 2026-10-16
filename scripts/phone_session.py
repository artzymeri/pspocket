#!/usr/bin/env python3
"""Summarises the iPhone app's sessions in a device log (idevicesyslog -p PSPocket output, D-149).

    phone_session.py SYSLOG [--since HH:MM]

One block per app process: how long it ran and how it ended, fps (median, slowest window, share of windows under 25),
the sound (slowest pace, blocks skipped), memory (most in use, least left, iOS's warnings and the backend's trims),
heat, the GPU's report windows (GPU ms, draws, passes, feedback copies, compiles in play), the busiest threads, and
freezes or fatal lines.
"""

import re
import statistics
import sys
from collections import defaultdict

LINE = re.compile(r"^\w+ +\d+ (\d\d:\d\d:\d\d)\S* PSPocket(?:\([^)]*\))?\[(\d+)\] <(\w+)>: (.*)$")
FPS = re.compile(r"([0-9.]+) fps \(.*heat (\w+)(?:; memory ([0-9.]+) GB, ([0-9.]+) GB left)?")
SOUND = re.compile(r"sound: .*?(\d+) skipped.*?the game's pace ([0-9.]+)")
WINDOW = re.compile(r"per frame \(last \d+\): GPU ([0-9.]+) ms, (\d+) draws, (\d+) render passes.*?"
                    r"(\d+) programs and (\d+) pipelines made \(([0-9.]+) ms\)")
FEEDBACK = re.compile(r"([0-9.]+) feedback copies")
GPU_MEM = re.compile(r"GPU memory (\d+) MB")
THREADS = re.compile(r"threads: CPU ([0-9.]+) s in ([0-9.]+) s \(([0-9.]+) cores\)[^:]*: (.*)")
HEAT_ORDER = ["nominal", "fair", "serious", "critical"]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    since = argv[argv.index("--since") + 1] if "--since" in argv else None
    sessions = defaultdict(lambda: defaultdict(list))
    order = []
    for raw in open(argv[1], errors="replace"):
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        t, pid, level, msg = m.groups()
        if since and t < since:
            continue
        s = sessions[pid]
        if pid not in order:
            order.append(pid)
            s["start"] = [t]
        s["end"] = [t]
        if (f := FPS.search(msg)) and "fps (" in msg:
            s["fps"].append(float(f.group(1)))
            s["heat"].append(f.group(2))
            if f.group(3):
                s["mem"].append(float(f.group(3)))
                s["left"].append(float(f.group(4)))
        elif (f := SOUND.search(msg)):
            s["skipped"].append(int(f.group(1)))
            s["pace"].append(float(f.group(2)))
        elif (f := WINDOW.search(msg)):
            s["gpu"].append(float(f.group(1)))
            s["draws"].append(int(f.group(2)))
            s["passes"].append(int(f.group(3)))
            s["made"].append(int(f.group(4)) + int(f.group(5)))
            s["compile_ms"].append(float(f.group(6)))
            if (c := FEEDBACK.search(msg)):
                s["feedback"].append(float(c.group(1)))
            if (g := GPU_MEM.search(msg)):
                s["gpu_mem"].append(int(g.group(1)))
        elif (f := THREADS.search(msg)):
            s["cores"].append(float(f.group(3)))
            s["threads"] = [f.group(4)]
        elif "Received memory warning" in msg:
            s["warnings"].append(t)
        elif msg.startswith("memory low:"):
            s["trims"].append(t)
        elif "freeze:" in msg or "deadlock:" in msg or "livelock:" in msg or "[fatal]" in msg or level == "Fault":
            s["bad"].append(f"{t} {msg[:160]}")
        if "shader cache:" in msg and "made ahead in" in msg:
            s["cache"] = [msg[msg.index("shader cache:"):][:120]]
    for pid in order:
        s = sessions[pid]
        if not s["fps"]:
            continue
        fps = s["fps"]
        print(f"== session {pid}: {s['start'][0]} to {s['end'][0]} (last line)")
        print(f"   fps: median {statistics.median(fps):.1f}, slowest window {min(fps):.1f}, "
              f"{100 * sum(f < 25 for f in fps) / len(fps):.0f}% of {len(fps)} windows under 25")
        if s["pace"]:
            print(f"   sound: slowest pace {min(s['pace']):.2f}, median {statistics.median(s['pace']):.2f}, "
                  f"{sum(s['skipped'])} blocks skipped")
        if s["mem"]:
            print(f"   memory: up to {max(s['mem']):.2f} GB, least left {min(s['left']):.2f} GB; "
                  f"{len(s['warnings'])} memory warnings, {len(s['trims'])} trims")
        elif s["warnings"]:
            print(f"   memory: {len(s['warnings'])} memory warnings (no readings in this build)")
        heat = max(s["heat"], key=lambda h: HEAT_ORDER.index(h) if h in HEAT_ORDER else 0)
        print(f"   heat: up to {heat}")
        if s["gpu"]:
            print(f"   GPU windows: {len(s['gpu'])}; GPU ms up to {max(s['gpu']):.1f}, draws up to {max(s['draws'])}, "
                  f"passes up to {max(s['passes'])}; compiled on the RSX thread {sum(s['made'])} "
                  f"({sum(s['compile_ms']) / 1000:.1f} s)"
                  + (f"; feedback copies up to {max(s['feedback']):.2f}/frame" if s["feedback"] else "")
                  + (f"; GPU memory up to {max(s['gpu_mem'])} MB" if s["gpu_mem"] else ""))
        if s["cores"]:
            print(f"   CPU: {statistics.median(s['cores']):.1f} cores (median); last: {s['threads'][0][:200]}")
        if s["cache"]:
            print(f"   {s['cache'][0]}")
        for b in s["bad"][:5]:
            print(f"   ! {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
